#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QStackedWidget>
#include <QStyleHints>

#include "appearance.h"
#include "CatalogReview.h"
#include "editors.h"
#include "GithubTokenStore.h"
#include "LibrarySplitter.h"
#include "LibraryViews.h"
#include "localization.h"
#include "MainWindow.h"
#include "NotificationBanner.h"
#include "ProfileView.h"
#include "SettingsView.h"
#include "UiHelpers.h"

namespace {
using namespace ui_helpers;

QWidget *shell(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = new QFrame(parent);
    widget->setObjectName("shell");
    auto *root = new QHBoxLayout(widget);
    root->setContentsMargins(16, 8, 16, 16);
    root->setSpacing(0);
    auto *navigation = context.registry->create("navigation", context, widget);
    auto *center = new QFrame(widget);
    center->setObjectName("library_workspace");
    auto *body = new QVBoxLayout(center);
    body->setContentsMargins(16, 16, 16, 12);
    body->setSpacing(0);
    auto *search_bar = context.registry->create("search_bar", context, center);
    body->addWidget(search_bar);
    body->addSpacing(20);
    auto *pages = new QStackedWidget;
    pages->addWidget(context.registry->create("library", context, pages));
    pages->addWidget(context.registry->create("scenarios", context, pages));
    pages->addWidget(context.registry->create("my_files", context, pages));
    pages->addWidget(context.registry->create("file_workspace", context, pages));
    pages->addWidget(context.registry->create("workspaces", context, pages));
    auto *transition = new PageTransition(pages);
    body->addWidget(transition, 1);
    auto *notice = new NotificationBanner(*context.settings, 20);
    QObject::connect(service, &DesktopService::appearance_changed, notice, &NotificationBanner::refresh_appearance);
    body->addWidget(notice);
    const auto show_notice = [=](const QString &text) { notice->show_message(text); };
    QObject::connect(service, &DesktopService::message, notice, show_notice);
    QObject::connect(service, &DesktopService::saved_files_notice, notice, [=](const QString &text, bool link) {
        notice->activated = [=] { service->navigate("files"); };
        notice->show_message(text, {}, link ? QCoreApplication::translate("XLibrary", "My files") : QString{});
    });
    QObject::connect(service, &DesktopService::scenario_changed, notice, show_notice);
    QObject::connect(service, &DesktopService::undo_removal_changed, notice, [=] {
        if (service->can_undo_removal()) {
            notice->activated = [=] { service->undo_removal(); };
            notice->show_message(QCoreApplication::translate("XLibrary", "Applications removed from the library."), {},
                                 QCoreApplication::translate("XLibrary", "Undo"));
        }
    });
    auto *update_notice = new NotificationBanner(*context.settings, 20);
    body->addWidget(update_notice);
    QObject::connect(service, &DesktopService::appearance_changed, update_notice,
                     &NotificationBanner::refresh_appearance);
    QObject::connect(&service->updates, &UpdateChecker::check_started, update_notice, [=] {
        update_notice->show_message(QCoreApplication::translate("XLibrary", "Checking for updates…"), {}, {}, false);
    });
    QObject::connect(&service->updates, &UpdateChecker::update_available, update_notice, [=] {
        update_notice->show_message(
            QCoreApplication::translate("XLibrary", "New version available: %1")
                .arg(service->updates.available_version()),
            QCoreApplication::translate("XLibrary", "Current version: %1").arg(QCoreApplication::applicationVersion()),
            QCoreApplication::translate("XLibrary", "Open update settings"));
    });
    QObject::connect(&service->updates, &UpdateChecker::up_to_date, update_notice, [=](const bool releases_exist) {
        update_notice->show_message(
            releases_exist
                ? QCoreApplication::translate("XLibrary", "You are using the latest available version.")
                : QCoreApplication::translate("XLibrary", "No releases with a supported version were found."),
            QCoreApplication::translate("XLibrary", "Current version: %1").arg(QCoreApplication::applicationVersion()));
    });
    QObject::connect(&service->updates, &UpdateChecker::failed, update_notice,
                     [=](const QString &) { update_notice->hide(); });
    QObject::connect(&service->updates, &UpdateChecker::check_cancelled, update_notice, [=] {
        update_notice->show_message(QCoreApplication::translate("XLibrary", "Update cancelled."));
    });
    QObject::connect(&service->updates, &UpdateChecker::channel_changed, update_notice, &QWidget::hide);
    update_notice->activated = [=] { context.actions.value("update_settings")->trigger(); };
    auto *splitter =
        new LibrarySplitter(*context.settings, navigation, center, context.registry->create("details", context, widget),
                            widget, [service] { return !service->profile_busy() && !service->restart_required(); });
    root->addWidget(splitter);
    QObject::connect(service, &DesktopService::navigation_changed, widget, [=] {
        const auto section = service->section();
        const bool show_search = section != "workspaces";
        if (search_bar->isHidden() == show_search)
            search_bar->setVisible(show_search);
        const int next = service->file_search_active() ? 3
                         : section == "scenarios"      ? 1
                         : section == "files"          ? 2
                         : section == "workspaces"     ? 4
                                                       : 0;
        if (next == pages->currentIndex())
            return;
        const int direction = next == 3 ? 1 : pages->currentIndex() == 3 ? -1 : 0;
        transition->transition_to([=] { pages->setCurrentIndex(next); }, direction, next == 3);
    });
    return widget;
}
}

MainWindow::MainWindow(DesktopService &service)
    : service(service), hotkey(this), overlay_hotkey(this), overlay(std::make_unique<SearchOverlay>(service)),
      tray(this) {
    if (!service.settings.contains("shortcuts/overlay")) {
        // The previous default now belongs to quick search; preserve other user assignments.
        if (service.settings.value("shortcuts/global").toString() == "Ctrl+Alt+Space")
            service.settings.setValue("shortcuts/global", desktop_platform::default_global_shortcut());
        service.settings.setValue("shortcuts/overlay", desktop_platform::default_overlay_shortcut());
    }
    connect(service.files, &FileManager::configuration_changed, this, [this] {
        refresh_actions();
        update_layout();
    });
    setWindowTitle(QGuiApplication::applicationDisplayName());
    setWindowIcon(appearance::logo());
    const auto available = screen()->availableGeometry().size() - QSize(32, 64);
    setMinimumSize(QSize(960, 640).boundedTo(available));
    resize(QSize(1280, 800).boundedTo(available));
    context.service = &service;
    context.catalog = &service.model;
    context.filtered_catalog = &service.filter;
    context.settings = &service.settings;
    context.registry = &registry;
    context.window = this;
    context.parameters.insert("hotkey", QVariant::fromValue(static_cast<QObject *>(&hotkey)));
    context.parameters.insert("overlay_hotkey", QVariant::fromValue(static_cast<QObject *>(&overlay_hotkey)));
    register_editors(registry);
    setAcceptDrops(true);
    connect(&service, &DesktopService::catalog_preview_ready, this, [this] { show_catalog_review(context); });
    register_library_views(registry);
    register_settings_views(registry);
    registry.add_default("shell", shell);
    create_actions();
    load_plugins();
    appearance::apply(service.settings, registry.style_sheet());
    setCentralWidget(registry.create("shell", context, this));
    auto *tray_menu = new QMenu(this);
    auto *show_action = tray_menu->addAction(QCoreApplication::translate("XLibrary", "Open Fora.Deck"));
    connect(show_action, &QAction::triggered, this, &MainWindow::show_library);
    tray_menu->addAction(context.actions.value("settings"));
    tray_menu->addSeparator();
    tray_menu->addAction(context.actions.value("quit"));
    tray.setIcon(windowIcon());
    tray.setToolTip(QGuiApplication::applicationDisplayName());
    tray.setContextMenu(tray_menu);
    if (QSystemTrayIcon::isSystemTrayAvailable())
        tray.show();
    connect(&tray, &QSystemTrayIcon::activated, this, [this](const QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            show_library();
    });
    connect(&hotkey, &GlobalHotkey::registration_failed, &service, &DesktopService::notify);
    connect(&overlay_hotkey, &GlobalHotkey::registration_failed, &service, &DesktopService::notify);
    connect(&hotkey, &GlobalHotkey::activated, this, &MainWindow::show_library);
    connect(&overlay_hotkey, &GlobalHotkey::activated, overlay.get(), &SearchOverlay::toggle);
    connect(overlay.get(), &SearchOverlay::open_library, this, [this](const QString &query) {
        show_library();
        this->service.navigate("library");
        if (auto *search = findChild<QLineEdit *>("search_input"))
            search->setText(query);
    });
    connect(&service, &DesktopService::selection_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::file_selection_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::catalog_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::profile_state_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::discovery_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::discovery_progress_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::navigation_changed, this, &MainWindow::update_layout);
    connect(&service, &DesktopService::navigation_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::appearance_changed, this, &MainWindow::apply_settings);
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (this->service.settings.value("appearance/theme").toString() == "system")
            emit this->service.appearance_changed();
    });
    connect(&service, &DesktopService::launch_succeeded, this, [this] {
        const auto behavior = this->service.settings.value("behavior/after_launch", "keep").toString();
        if (behavior == "minimize")
            showMinimized();
        // The old hide-to-tray preference now keeps the library available in the taskbar.
        else if (isVisible() && !isMinimized())
            lower();
    });
    if (!restoreGeometry(service.settings.value("window/geometry").toByteArray()))
        resize(QSize(1280, 800).boundedTo(available));
    apply_settings();
    refresh_actions();
    update_layout();
}

MainWindow::~MainWindow() {
    // Destroy plugin factories and widgets before unloading their code.
    delete takeCentralWidget();
    for (auto *dialog : findChildren<QDialog *>(QString{}, Qt::FindDirectChildrenOnly))
        delete dialog;
    registry = UiRegistry{};
}

void MainWindow::create_actions() {
    auto add = [this](const QString &id, const QString &title, const QString &icon, const auto &callback) {
        auto *action = new QAction(appearance::icon(icon), title, this);
        action->setObjectName(id);
        action->setProperty("appearance_icon", icon);
        context.actions.insert(id, action);
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    add("launch", QCoreApplication::translate("XLibrary", "Launch"), "play", [this] {
        if (service.selection())
            service.launch(service.selection());
    });
    add("new_instance", QCoreApplication::translate("XLibrary", "New instance"), "new_instance", [this] {
        if (service.selection())
            service.launch(service.selection(), true);
    });
    add("terminate", QCoreApplication::translate("XLibrary", "Terminate"), "stop",
        [this] { confirm_application_termination(service, service.selection(), this); });
    add("focus_search", QCoreApplication::translate("XLibrary", "Search"), "search", [] {});
    add("settings", QCoreApplication::translate("XLibrary", "Settings"), "settings", [this] {
        if (!isVisible())
            show_library();
        open_dialog("settings", context);
    });
    add("update_settings", QCoreApplication::translate("XLibrary", "Update"), "refresh", [this] {
        if (!isVisible())
            show_library();
        auto update_context = context;
        update_context.parameters.insert("settings_section", "settings_update");
        if (auto *existing = findChild<QWidget *>("settings", Qt::FindDirectChildrenOnly))
            if (auto *category = existing->findChild<QAbstractButton *>("settings_update_category"))
                category->click();
        open_dialog("settings", update_context);
    });
    add("add_application", QCoreApplication::translate("XLibrary", "Add application"), "add",
        [this] { open_dialog("application_editor", context); });
    add("add_items", QCoreApplication::translate("XLibrary", "Add"), "add", [this] { open_add_items(); });
    add("welcome", QCoreApplication::translate("XLibrary", "Open welcome screen"), "app", [this] { show_welcome(); });
    add("tutorial", QCoreApplication::translate("XLibrary", "Take a quick tour"), "help", [this] { show_tutorial(); });
    add("edit_application", QCoreApplication::translate("XLibrary", "Edit"), "edit", [this] {
        if (const auto *record = service.selected()) {
            auto edit_context = context;
            edit_context.parameters.insert("record", QVariant::fromValue(*record));
            open_dialog("application_editor", edit_context);
        }
    });
    add("remove_application", QCoreApplication::translate("XLibrary", "Remove from library"), "trash", [this] {
        const auto *record = service.selected();
        if (!record)
            return;
        const auto ids = service.selections().isEmpty() ? QVector<qint64>{record->id} : service.selections();
        if (confirm(this, QCoreApplication::translate("XLibrary", "Remove application"),
                    QCoreApplication::translate("XLibrary",
                                                "Remove %1 applications from the library? Application files will be "
                                                "kept. Linked scenario steps will be removed; Undo can restore them.")
                        .arg(ids.size())))
            service.remove_applications(ids);
    });
    context.actions.value("remove_application")
        ->setToolTip(QCoreApplication::translate(
            "XLibrary", "Remove the selected application from the library. Application files will be kept."));
    add("favorite", QCoreApplication::translate("XLibrary", "Add to favorites"), "favorite", [this] {
        auto *action = context.actions.value("favorite");
        action->setProperty("favorite_request_id", {});
        if (const auto *record = service.selected()) {
            auto updated = *record;
            updated.favorite = !updated.favorite;
            action->setProperty("favorite_request_id",
                                QVariant::fromValue(service.save_application(std::move(updated))));
        }
    });
    add("open_folder", QCoreApplication::translate("XLibrary", "Open folder"), "folder",
        [this] { service.open_folder(service.selection()); });
    add("copy_path", QCoreApplication::translate("XLibrary", "Copy path"), "copy", [this] {
        if (const auto *record = service.selected()) {
            const auto path = Application::application_path(*record);
            const bool local_path =
                Application::file_target(*record) || (Application::target_kind(*record) == "steam" && !path.isEmpty());
            QApplication::clipboard()->setText(local_path ? QDir::toNativeSeparators(path) : record->path);
        }
    });
    add("copy_arguments", QCoreApplication::translate("XLibrary", "Copy arguments"), "copy", [this] {
        if (const auto *record = service.selected())
            QApplication::clipboard()->setText(record->arguments);
    });
    add("bulk_edit", QCoreApplication::translate("XLibrary", "Tags"), "tag",
        [this] { show_application_tags(context); });
    add("undo_removal", QCoreApplication::translate("XLibrary", "Undo removal"), "undo",
        [this] { service.undo_removal(); });
    connect(&service, &DesktopService::undo_removal_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::selections_changed, this, &MainWindow::refresh_actions);
    for (const auto &id : {"launch", "new_instance", "open_folder", "copy_path", "remove_application"})
        context.actions.value(id)->setAutoRepeat(false);
    add("refresh_availability", QCoreApplication::translate("XLibrary", "Refresh availability"), "refresh",
        [this] { service.refresh_availability(); });
    add("library_health", QCoreApplication::translate("XLibrary", "Library health"), "info",
        [this] { show_library_health(context); });
    add("discover", QCoreApplication::translate("XLibrary", "Find installed applications"), "scan", [this] {
        if (service.is_discovering())
            service.cancel_discovery();
        else
            service.discover();
    });
    add("add_tag", QCoreApplication::translate("XLibrary", "Add tag"), "add",
        [this] { open_dialog("tag_editor", context); });
    add("add_scenario", QCoreApplication::translate("XLibrary", "Create scenario"), "add",
        [this] { open_dialog("scenario_editor", context); });
    add("details", QCoreApplication::translate("XLibrary", "Details"), "app",
        [this] { open_dialog("details_dialog", context); });
    add("quit", QCoreApplication::translate("XLibrary", "Quit"), "x", [this] {
        quitting = true;
        close();
        qApp->quit();
    });
    auto *escape = new QAction(this);
    escape->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(escape);
    connect(escape, &QAction::triggered, this, [this] {
        if (auto *search = findChild<QLineEdit *>("search_input"))
            search->clear();
    });
}

void MainWindow::refresh_actions() {
    const auto *record = service.selected();
    const bool profile_available = !service.profile_busy() && !service.restart_required();
    for (const auto &id : {"launch", "new_instance", "terminate", "edit_application", "remove_application", "favorite",
                           "open_folder", "copy_path", "copy_arguments", "details"})
        context.actions.value(id)->setEnabled(profile_available && record != nullptr &&
                                              (service.section() != "scenarios" && service.section() != "workspaces") &&
                                              service.section() != "files" && !service.file_search_active());
    if (service.file_search_active() || service.section() == "files")
        context.actions.value("details")->setEnabled(profile_available && !service.selected_file().isEmpty());
    auto *terminate = context.actions.value("terminate");
    terminate->setVisible(record && record->running &&
                          (service.section() != "scenarios" && service.section() != "workspaces") &&
                          service.section() != "files" && !service.file_search_active());
    terminate->setEnabled(terminate->isEnabled() && record && record->running);
    const bool folder_supported =
        record && (Application::file_target(*record) || !record->installation_directory.isEmpty());
    auto *open_folder = context.actions.value("open_folder");
    open_folder->setEnabled(open_folder->isEnabled() && folder_supported);
    const bool local_path =
        record && (Application::file_target(*record) ||
                   (Application::target_kind(*record) == "steam" && !Application::application_path(*record).isEmpty()));
    context.actions.value("copy_path")
        ->setText(!record || local_path ? QCoreApplication::translate("XLibrary", "Copy path")
                                        : QCoreApplication::translate("XLibrary", "Copy launch target"));
    context.actions.value("launch")->setEnabled(context.actions.value("launch")->isEnabled() && record &&
                                                record->available);
    auto *new_instance = context.actions.value("new_instance");
    const bool can_start = profile_available && record && record->available && record->new_instance_error.isEmpty();
    new_instance->setEnabled(can_start && (service.section() != "scenarios" && service.section() != "workspaces") &&
                             service.section() != "files" && !service.file_search_active());
    new_instance->setText(QCoreApplication::translate("XLibrary", "Request new instance"));
    new_instance->setToolTip(
        record && !record->new_instance_error.isEmpty()
            ? record->new_instance_error
            : QCoreApplication::translate(
                  "XLibrary", "Runs the launch command directly. The application may reuse an existing window."));
    context.actions.value("add_application")->setEnabled(profile_available && service.is_ready());
    context.actions.value("add_items")->setEnabled(can_add_items());
    context.actions.value("refresh_availability")->setEnabled(profile_available && service.is_ready());
    context.actions.value("bulk_edit")
        ->setEnabled(profile_available && service.is_ready() && !service.file_search_active() &&
                     !service.selections().isEmpty());
    context.actions.value("undo_removal")
        ->setEnabled(profile_available && service.can_undo_removal() && !service.is_scenario_active());
    context.actions.value("add_tag")->setEnabled(profile_available && service.is_ready());
    context.actions.value("add_scenario")
        ->setEnabled(profile_available && service.is_ready() && service.model.rowCount() > 0);
    context.actions.value("discover")
        ->setEnabled(profile_available && service.is_ready() &&
                     (!service.is_discovering() || service.can_cancel_discovery()));
    context.actions.value("discover")
        ->setText(service.is_discovering() ? QCoreApplication::translate("XLibrary", "Cancel discovery")
                                           : QCoreApplication::translate("XLibrary", "Find installed applications"));
    context.actions.value("discover")->setIcon(appearance::icon(service.is_discovering() ? "stop" : "scan"));
    context.actions.value("launch")->setText(
        record && record->running && service.settings.value("behavior/relaunch", "activate").toString() == "activate"
            ? QCoreApplication::translate("XLibrary", "Switch to window")
            : QCoreApplication::translate("XLibrary", "Launch"));
    context.actions.value("favorite")
        ->setText(record && record->favorite ? QCoreApplication::translate("XLibrary", "Remove from favorites")
                                             : QCoreApplication::translate("XLibrary", "Add to favorites"));
    context.actions.value("favorite")
        ->setIcon(
            appearance::icon(record && record->favorite ? "favorite_filled" : "favorite",
                             palette().color(record && record->favorite ? QPalette::Link : QPalette::PlaceholderText)));
}

void MainWindow::apply_settings() {
    appearance::apply(service.settings, registry.style_sheet());
    for (auto *action : context.actions)
        if (const auto icon_name = action->property("appearance_icon").toString(); !icon_name.isEmpty())
            action->setIcon(appearance::icon(icon_name));
    for (auto *control : findChildren<QAbstractButton *>()) {
        const auto icon_name = control->property("appearance_icon").toString();
        if (!icon_name.isEmpty())
            control->setIcon(appearance::icon(icon_name, control->property("primary").toBool()
                                                             ? QColor("#181A1D")
                                                             : control->palette().color(QPalette::PlaceholderText)));
        control->setIconSize(QSize(appearance::scaled(service.settings, 20), appearance::scaled(service.settings, 20)));
        if (control->property("icon_only").toBool())
            control->setFixedWidth(appearance::scaled(service.settings, 44));
    }
    QString error;
    if (!hotkey.set_sequence(
            QKeySequence::fromString(
                service.settings.value("shortcuts/global", desktop_platform::default_global_shortcut()).toString(),
                QKeySequence::PortableText),
            error))
        service.notify(error);
    if (!overlay_hotkey.set_sequence(
            QKeySequence::fromString(
                service.settings.value("shortcuts/overlay", desktop_platform::default_overlay_shortcut()).toString(),
                QKeySequence::PortableText),
            error))
        service.notify(error);
    const QList<QStringList> shortcuts{{"focus_search", "shortcuts/search", "Ctrl+K"},
                                       {"add_items", "shortcuts/add", "Ctrl+N"},
                                       {"settings", "shortcuts/settings", "Ctrl+,"},
                                       {"launch", "shortcuts/launch", "Return"},
                                       {"new_instance", "shortcuts/new_instance", "Ctrl+Return"},
                                       {"open_folder", "shortcuts/open_folder", "Alt+Return"},
                                       {"copy_path", "shortcuts/copy_path", "Ctrl+Shift+C"},
                                       {"remove_application", "shortcuts/remove", "Delete"}};
    for (const auto &value : shortcuts)
        context.actions.value(value[0])->setShortcuts(shortcut_sequences(QKeySequence::fromString(
            service.settings.value(value[1], value[2]).toString(), QKeySequence::PortableText)));
    desktop_platform::apply_backdrop(this, appearance::is_dark(service.settings),
                                     service.settings.value("appearance/glass", false).toBool());
    refresh_actions();
    update_layout();
}

void MainWindow::load_plugins() {
    if (service.safe_mode()) {
        context.parameters.insert("available_plugins", QStringList{});
        return;
    }
    const QDir directory(QApplication::applicationDirPath() + "/plugins");
    QStringList available;
    QVariantMap metadata;
    QVariantMap errors;
    const auto enabled = service.settings.value("plugins/enabled").toStringList();
    const auto report_error = [this, &errors](const QString &id, const QString &error) {
        errors.insert(id, error);
        if (service.settings.value("plugins/enabled").toStringList().contains(id))
            QTimer::singleShot(0, &service, [this, id, error] {
                service.notify(QCoreApplication::translate("XLibrary", "Could not load ") + id + ": " + error);
            });
    };
    for (const auto &file : directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        if (!QLibrary::isLibrary(file.fileName()))
            continue;
        const auto id = file.fileName();
        available.append(id);
        try {
            auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
            const auto embedded = loader->metaData();
            metadata.insert(id, embedded.value("MetaData").toObject().toVariantMap());
            const auto compatibility_error = UiRegistry::plugin_compatibility_error(embedded);
            if (!compatibility_error.isEmpty()) {
                report_error(id, compatibility_error);
                continue;
            }
            if (!enabled.contains(id))
                continue;
            // Registry callbacks may outlive a failed transaction or the loader itself.
            loader->setLoadHints(QLibrary::PreventUnloadHint);
            auto *plugin = qobject_cast<UiPlugin *>(loader->instance());
            if (!plugin) {
                report_error(id, loader->errorString());
                continue;
            }
            // Allocate loader ownership before publishing registry callbacks.
            plugins.push_back(std::move(loader));
            const auto error =
                registry.configure_transaction([plugin](UiRegistry &candidate) { plugin->configure(candidate); });
            if (!error.isEmpty())
                report_error(id, error);
        } catch (const std::exception &error) {
            report_error(id, QString::fromUtf8(error.what()));
        } catch (...) {
            report_error(id, QCoreApplication::translate("XLibrary", "Unknown C++ exception while loading plugin."));
        }
    }
    context.parameters.insert("available_plugins", available);
    context.parameters.insert("plugin_metadata", metadata);
    context.parameters.insert("plugin_errors", errors);
}

void MainWindow::update_layout() {
    // A custom plugin shell manages its own layout.
    if (centralWidget())
        for (auto *splitter : centralWidget()->findChildren<QSplitter *>("library_splitter"))
            if (auto *library = dynamic_cast<LibrarySplitter *>(splitter))
                library->update_layout((service.section() != "scenarios" && service.section() != "workspaces"));
}

void MainWindow::show_library() {
    overlay->hide();
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
    context.actions.value("focus_search")->trigger();
}

void MainWindow::show_overlay() {
    overlay->show_search();
}

void MainWindow::offer_brand_transition() {
    if (!isVisible() || !service.is_ready() || service.profile_busy() || service.restart_required() ||
        QCoreApplication::arguments().contains("--check-startup") ||
        service.settings.value("onboarding/fora_deck_announced", false).toBool())
        return;
    const bool existing_profile =
        service.settings.value("onboarding/completed", false).toBool() || !service.catalog().applications.isEmpty();
    service.settings.setValue("onboarding/fora_deck_announced", true);
    if (existing_profile)
        service.notify(QCoreApplication::translate(
            "XLibrary", "XLibrary is now Fora.Deck. Your library and settings have been preserved."));
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::offer_brand_transition);
}

void MainWindow::start() {
    connect(&service, &DesktopService::catalog_changed, this, &MainWindow::offer_brand_transition);
    const auto offer_welcome = [this] {
        if (welcome_offered || !service.is_ready() || service.profile_busy() || service.restart_required() ||
            QCoreApplication::arguments().contains("--check-startup"))
            return;
        welcome_offered = true;
        if (!service.settings.value("onboarding/completed", false).toBool())
            show_welcome();
    };
    connect(&service, &DesktopService::catalog_changed, this, offer_welcome);
    QTimer::singleShot(0, this, offer_welcome);
    if (service.safe_mode() && !QCoreApplication::arguments().contains("--check-startup"))
        QTimer::singleShot(0, &service, [this] {
            service.notify(
                QCoreApplication::translate("XLibrary", "Safe mode: user plugins are disabled for this session."));
        });
    const bool background = QCoreApplication::arguments().contains("--background") ||
                            service.settings.value("behavior/startup", "show").toString() == "tray";
    if (QCoreApplication::arguments().contains("--overlay") &&
        !QCoreApplication::arguments().contains("--check-startup"))
        show_overlay();
    else if (service.safe_mode() || !background || !tray.isVisible())
        show_library();
    if (!QCoreApplication::arguments().contains("--check-startup")) {
        QTimer::singleShot(0, this, [this] {
            if (service.updates.is_busy())
                return;
            auto *tokens = new GithubTokenStore(service.settings, &service);
            connect(tokens, &GithubTokenStore::read_finished, &service.updates, &UpdateChecker::check);
            connect(tokens, &GithubTokenStore::failed, &service, &DesktopService::notify);
            connect(tokens, &GithubTokenStore::read_finished, tokens, &QObject::deleteLater);
            connect(tokens, &GithubTokenStore::failed, tokens, &QObject::deleteLater);
            tokens->read();
        });
    }
}

void MainWindow::show_welcome() {
    if (!service.is_ready() || service.profile_busy() || service.restart_required())
        return;
    if (auto *existing = findChild<QDialog *>("welcome", Qt::FindDirectChildrenOnly)) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    show_library();
    auto *dialog = new QDialog(this);
    dialog->setObjectName("welcome");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "Welcome to Fora.Deck"));
    prepare_dialog(dialog, context, {700, 640}, {480, 400});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(28, 28, 28, 28);
    root->setSpacing(16);
    auto *logo = new QLabel;
    const auto logo_size = appearance::scaled(service.settings, 64);
    logo->setPixmap(appearance::logo().pixmap(logo_size, logo_size));
    root->addWidget(logo, 0, Qt::AlignHCenter);
    root->addWidget(label(QGuiApplication::applicationDisplayName(), "heading"), 0, Qt::AlignHCenter);
    root->addWidget(label(QCoreApplication::translate("XLibrary", "Your applications. One home."), "heading"));
    root->addWidget(label(
        QCoreApplication::translate("XLibrary", "Choose how to start. You can return to this screen in Settings."),
        "muted"));
    auto *contents = new QWidget;
    auto *cards = new QVBoxLayout(contents);
    cards->setContentsMargins(0, 0, 0, 0);
    const auto choice = [=, this](const QString &title, const QString &description, const QString &icon,
                                  const std::function<void()> &callback) {
        auto *card = new QFrame;
        card->setProperty("card", true);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 12, 16, 12);
        auto *control = button(title, nullptr, icon);
        layout->addWidget(control);
        layout->addWidget(label(description, "muted"));
        cards->addWidget(card);
        connect(control, &QPushButton::clicked, dialog, [=, this] {
            dialog->accept();
            QTimer::singleShot(0, this, callback);
        });
    };
    choice(QCoreApplication::translate("XLibrary", "Find my applications"),
           QCoreApplication::translate("XLibrary", "Review installed applications and choose what to keep."), "scan",
           [this] { service.discover(); });
    choice(QCoreApplication::translate("XLibrary", "Add an application manually"),
           QCoreApplication::translate("XLibrary", "Choose a file or shortcut to add it to your library."), "add",
           [this] { context.actions.value("add_application")->trigger(); });
    choice(QCoreApplication::translate("XLibrary", "Restore my library"),
           QCoreApplication::translate("XLibrary", "Preview and restore a complete .xlprofile backup."), "folder",
           [this] { select_profile_restore(context, this); });
    root->addWidget(ui_helpers::scroll(contents), 1);
    auto *later = button(QCoreApplication::translate("XLibrary", "Start with an empty library"), "quiet");
    auto *tour = button(QCoreApplication::translate("XLibrary", "Take a quick tour"));
    tour->setIcon(appearance::icon("help"));
    root->addWidget(tour);
    connect(tour, &QPushButton::clicked, dialog, [this, dialog] {
        dialog->accept();
        QTimer::singleShot(0, this, &MainWindow::show_tutorial);
    });
    root->addWidget(later);
    connect(later, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this] {
        if (!service.profile_busy() && !service.restart_required()) {
            service.settings.setValue("onboarding/completed", true);
            service.settings.sync();
        }
    });
    dialog->setWindowModality(Qt::WindowModal);
    dialog->open();
}

void MainWindow::show_tutorial() {
    if (auto *existing = findChild<QDialog *>("tutorial")) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *parent = QApplication::activeModalWidget();
    auto *dialog = new QDialog(parent ? parent : this);
    dialog->setObjectName("tutorial");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "A quick tour of Fora.Deck"));
    prepare_dialog(dialog, context, {640, 500}, {420, 340});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);
    auto *progress = label("", "muted");
    root->addWidget(progress);
    auto *pages = new QStackedWidget;
    const QList<QStringList> topics{
        {"search", QCoreApplication::translate("XLibrary", "Find it in a few keystrokes"),
         QCoreApplication::translate("XLibrary",
                                     "Search by name, alias or filename. Add #work to narrow results or -#games to "
                                     "exclude a tag. Quick search can also browse a path or calculate an expression."),
         QStringLiteral("#work browser     ·     (2 + 3) * 4")},
        {"library", QCoreApplication::translate("XLibrary", "Make the library yours"),
         QCoreApplication::translate(
             "XLibrary",
             "Add an application from a file or find installed applications. Edit its name, arguments and tags. Use "
             "Ctrl or Shift to select several entries and edit them together. Removing an entry keeps its files."),
         QCoreApplication::translate("XLibrary", "Add → Edit → Tags → Favorites")},
        {"scenarios", QCoreApplication::translate("XLibrary", "Open your workspace in order"),
         QCoreApplication::translate(
             "XLibrary",
             "Create a scenario, choose applications and set a delay before each launch. Review problems before "
             "starting and follow each step's status. Stop leaves already opened applications running."),
         QCoreApplication::translate("XLibrary", "Waiting → Delay → Launching → Success")},
        {"keyboard", QCoreApplication::translate("XLibrary", "Keep Fora.Deck close"),
         QCoreApplication::translate("XLibrary",
                                     "Settings → Keyboard shortcuts shows your current bindings for the library, quick "
                                     "search and individual applications or scenarios. Settings also contains local "
                                     "data information, profile backups and quick help."),
         QCoreApplication::translate("XLibrary", "Settings → Keyboard shortcuts / Quick help")}};
    for (const auto &topic : topics) {
        auto *contents = new QWidget;
        auto *layout = new QVBoxLayout(contents);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(20);
        auto *icon = new QLabel;
        const auto refresh_icon = [this, icon, icon_name = topic[0]] {
            const auto size = appearance::scaled(service.settings, 48);
            icon->setPixmap(appearance::icon(icon_name, palette().color(QPalette::WindowText)).pixmap(size, size));
        };
        connect(&service, &DesktopService::appearance_changed, icon, refresh_icon);
        refresh_icon();
        layout->addWidget(icon);
        layout->addWidget(label(topic[1], "heading"));
        layout->addWidget(label(topic[2], "muted"));
        auto *example = new QFrame;
        example->setProperty("card", true);
        auto *example_layout = new QVBoxLayout(example);
        example_layout->setContentsMargins(16, 16, 16, 16);
        example_layout->addWidget(label(topic[3], "subheading"));
        layout->addWidget(example);
        layout->addStretch();
        pages->addWidget(ui_helpers::scroll(contents));
    }
    auto *transition = new PageTransition(pages);
    root->addWidget(transition, 1);
    auto *controls = new QHBoxLayout;
    auto *back = button(QCoreApplication::translate("XLibrary", "Back"));
    auto *next = button("", "primary");
    controls->addWidget(back);
    controls->addStretch();
    controls->addWidget(next);
    root->addLayout(controls);
    const auto update = [=] {
        progress->setText(QCoreApplication::translate("XLibrary", "Step %1 of %2")
                              .arg(pages->currentIndex() + 1)
                              .arg(pages->count()));
        back->setEnabled(pages->currentIndex() > 0);
        next->setText(pages->currentIndex() + 1 == pages->count() ? QCoreApplication::translate("XLibrary", "Finish")
                                                                  : QCoreApplication::translate("XLibrary", "Next"));
    };
    connect(back, &QPushButton::clicked, dialog, [=] { pages->setCurrentIndex(pages->currentIndex() - 1); });
    connect(next, &QPushButton::clicked, dialog, [=] {
        if (pages->currentIndex() + 1 == pages->count())
            dialog->accept();
        else
            pages->setCurrentIndex(pages->currentIndex() + 1);
    });
    connect(pages, &QStackedWidget::currentChanged, dialog, [=] {
        update();
        transition->start();
    });
    update();
    dialog->setWindowModality(Qt::WindowModal);
    dialog->open();
}

void MainWindow::closeEvent(QCloseEvent *const event) {
    if (!service.profile_busy() && !service.restart_required()) {
        service.settings.setValue("window/geometry", saveGeometry());
        service.settings.sync();
    }
    if (!quitting && service.settings.value("behavior/close_to_tray", true).toBool() && tray.isVisible()) {
        hide();
        event->ignore();
        return;
    }
    quitting = true;
    event->accept();
    qApp->quit();
}

void MainWindow::resizeEvent(QResizeEvent *const event) {
    QMainWindow::resizeEvent(event);
    update_layout();
}

bool MainWindow::can_add_items() const {
    return service.is_ready() && !service.profile_busy() && !service.restart_required();
}

void MainWindow::open_add_items() {
    static const QString id = "add_items_dialog";
    const bool already_open = findChild<QDialog *>(id, Qt::FindDirectChildrenOnly);
    open_dialog(id, context);
    if (already_open)
        return;
    if (auto *dialog = findChild<QDialog *>(id, Qt::FindDirectChildrenOnly)) {
        connect(dialog, &QDialog::accepted, this, [this, dialog] {
            if (!can_add_items())
                return;
            if (dialog->property("manual_application").toBool())
                context.actions.value("add_application")->trigger();
            else
                review_add_items(dialog->property("selected_paths").toStringList());
        });
    }
}

void MainWindow::review_add_applications(const QStringList &paths) {
    if (!can_add_items() || paths.isEmpty())
        return;
    if (paths.size() > 1) {
        if (service.is_discovering())
            service.notify(QCoreApplication::translate(
                "XLibrary", "Wait for the current scan or import to finish, then try again."));
        else
            service.preview_files(paths);
        return;
    }
    auto editor_context = context;
    editor_context.parameters.insert("initial_application_path", paths.first());
    open_dialog("application_editor", editor_context);
}

void MainWindow::review_add_items(const QStringList &paths) {
    QStringList applications;
    QStringList documents;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        if (!FileManager::supports_file_extension(info.suffix()) && Application::is_application_file_candidate(info))
            applications.append(path);
        else
            documents.append(path);
    }
    applications.removeDuplicates();
    if (documents.isEmpty()) {
        review_add_applications(applications);
        return;
    }
    auto files_context = context;
    files_context.parameters.insert("initial_file_paths", documents);
    files_context.parameters.insert("pending_applications", !applications.isEmpty());
    open_dialog("add_files_dialog", files_context);
    if (!applications.isEmpty())
        if (auto *dialog = findChild<QDialog *>("add_files_dialog", Qt::FindDirectChildrenOnly))
            connect(
                dialog, &QDialog::accepted, this, [this, applications] { review_add_applications(applications); },
                Qt::SingleShotConnection);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *const event) {
    if (!can_add_items() || service.is_discovering() || local_drop_paths(*event).isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void MainWindow::dropEvent(QDropEvent *const event) {
    const auto paths = local_drop_paths(*event);
    if (!can_add_items() || service.is_discovering() || paths.isEmpty()) {
        event->ignore();
        return;
    }
    review_add_items(paths);
    event->setDropAction(Qt::CopyAction);
    event->accept();
}
