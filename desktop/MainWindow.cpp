#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
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
    auto *root = new QHBoxLayout(widget);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(0);
    auto *navigation = context.registry->create("navigation", context, widget);
    auto *center = new QFrame;
    center->setObjectName("library_workspace");
    auto *body = new QVBoxLayout(center);
    body->setContentsMargins(20, 16, 20, 16);
    body->setSpacing(0);
    body->addWidget(context.registry->create("search_bar", context, center));
    body->addSpacing(20);
    auto *pages = new QStackedWidget;
    pages->addWidget(context.registry->create("library", context, pages));
    pages->addWidget(context.registry->create("scenarios", context, pages));
    pages->addWidget(context.registry->create("my_files", context, pages));
    auto *transition = new PageTransition(pages);
    body->addWidget(transition, 1);
    auto *notice = new NotificationBanner(*context.settings, 20);
    QObject::connect(service, &DesktopService::appearance_changed, notice, &NotificationBanner::refresh_appearance);
    body->addWidget(notice);
    const auto show_notice = [=](const QString &text) { notice->show_message(text); };
    QObject::connect(service, &DesktopService::message, notice, show_notice);
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
    QObject::connect(service, &DesktopService::navigation_changed, widget,
                     [=, section = service->section(), tag = service->tag()]() mutable {
                         if (section == service->section() && tag == service->tag())
                             return;
                         section = service->section();
                         tag = service->tag();
                         pages->setCurrentIndex(section == "scenarios" ? 1 : section == "files" ? 2 : 0);
                         transition->start();
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
    setWindowTitle("XLibrary");
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
    auto *show_action = tray_menu->addAction(QCoreApplication::translate("XLibrary", "Open XLibrary"));
    connect(show_action, &QAction::triggered, this, &MainWindow::show_library);
    tray_menu->addAction(context.actions.value("settings"));
    tray_menu->addSeparator();
    tray_menu->addAction(context.actions.value("quit"));
    tray.setIcon(windowIcon());
    tray.setToolTip("XLibrary");
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
    connect(&service, &DesktopService::catalog_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::profile_state_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::discovery_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::discovery_progress_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::navigation_changed, this, &MainWindow::update_layout);
    connect(&service, &DesktopService::navigation_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::appearance_changed, this, &MainWindow::apply_settings);
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (this->service.settings.value("appearance/theme").toString() == "system")
            apply_settings();
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
        context.actions.insert(id, action);
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    add("launch", QCoreApplication::translate("XLibrary", "Launch"), "play", [this] {
        if (service.selection())
            service.launch(service.selection());
    });
    add("new_instance", QCoreApplication::translate("XLibrary", "New instance"), "play", [this] {
        if (service.selection())
            service.launch(service.selection(), true);
    });
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
    add("welcome", QCoreApplication::translate("XLibrary", "Open welcome screen"), "app", [this] { show_welcome(); });
    add("tutorial", QCoreApplication::translate("XLibrary", "Take a quick tour"), "help", [this] { show_tutorial(); });
    add("edit_application", QCoreApplication::translate("XLibrary", "Edit"), "settings", [this] {
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
        if (const auto *record = service.selected()) {
            auto updated = *record;
            updated.favorite = !updated.favorite;
            service.save_application(std::move(updated));
        }
    });
    add("open_folder", QCoreApplication::translate("XLibrary", "Open folder"), "folder",
        [this] { service.open_folder(service.selection()); });
    add("copy_path", QCoreApplication::translate("XLibrary", "Copy path"), "copy", [this] {
        if (const auto *record = service.selected())
            QApplication::clipboard()->setText(QDir::toNativeSeparators(Application::application_path(*record)));
    });
    add("copy_arguments", QCoreApplication::translate("XLibrary", "Copy arguments"), "copy", [this] {
        if (const auto *record = service.selected())
            QApplication::clipboard()->setText(record->arguments);
    });
    add("bulk_edit", QCoreApplication::translate("XLibrary", "Edit selected applications"), "settings",
        [this] { show_bulk_editor(context); });
    add("undo_removal", QCoreApplication::translate("XLibrary", "Undo removal"), "refresh",
        [this] { service.undo_removal(); });
    connect(&service, &DesktopService::undo_removal_changed, this, &MainWindow::refresh_actions);
    connect(&service, &DesktopService::selections_changed, this, &MainWindow::refresh_actions);
    for (const auto &id : {"launch", "new_instance", "open_folder", "copy_path", "remove_application"})
        context.actions.value(id)->setAutoRepeat(false);
    add("refresh_availability", QCoreApplication::translate("XLibrary", "Refresh availability"), "refresh",
        [this] { service.refresh_availability(); });
    add("discover", QCoreApplication::translate("XLibrary", "Find installed applications"), "refresh", [this] {
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
    for (const auto &id : {"launch", "new_instance", "edit_application", "remove_application", "favorite",
                           "open_folder", "copy_path", "copy_arguments", "details"})
        context.actions.value(id)->setEnabled(profile_available && record != nullptr &&
                                              service.section() != "scenarios" && service.section() != "files");
    auto *new_instance = context.actions.value("new_instance");
    const bool can_start = profile_available && record && record->available && record->new_instance_error.isEmpty();
    new_instance->setEnabled(can_start && service.section() != "scenarios" && service.section() != "files");
    new_instance->setText(QCoreApplication::translate("XLibrary", "Request new instance"));
    new_instance->setToolTip(
        record && !record->new_instance_error.isEmpty()
            ? record->new_instance_error
            : QCoreApplication::translate(
                  "XLibrary", "Runs the launch command directly. The application may reuse an existing window."));
    context.actions.value("add_application")->setEnabled(profile_available && service.is_ready());
    context.actions.value("refresh_availability")->setEnabled(profile_available && service.is_ready());
    context.actions.value("bulk_edit")
        ->setEnabled(profile_available && service.is_ready() && !service.selections().isEmpty());
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
    context.actions.value("launch")->setText(
        record && record->running && service.settings.value("behavior/relaunch", "activate").toString() == "activate"
            ? QCoreApplication::translate("XLibrary", "Switch to window")
            : QCoreApplication::translate("XLibrary", "Launch"));
    context.actions.value("favorite")
        ->setText(record && record->favorite ? QCoreApplication::translate("XLibrary", "Remove from favorites")
                                             : QCoreApplication::translate("XLibrary", "Add to favorites"));
}

void MainWindow::apply_settings() {
    appearance::apply(service.settings, registry.style_sheet());
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
                                       {"add_application", "shortcuts/add", "Ctrl+N"},
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
                library->update_layout(service.section() != "scenarios" && service.section() != "files");
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

void MainWindow::start() {
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
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "Welcome to XLibrary"));
    prepare_dialog(dialog, context, {700, 640}, {480, 400});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(28, 28, 28, 28);
    root->setSpacing(16);
    auto *logo = new QLabel;
    const auto logo_size = appearance::scaled(service.settings, 64);
    logo->setPixmap(appearance::logo().pixmap(logo_size, logo_size));
    root->addWidget(logo, 0, Qt::AlignHCenter);
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
        auto *control = button(title);
        control->setIcon(appearance::icon(icon));
        layout->addWidget(control);
        layout->addWidget(label(description, "muted"));
        cards->addWidget(card);
        connect(control, &QPushButton::clicked, dialog, [=, this] {
            dialog->accept();
            QTimer::singleShot(0, this, callback);
        });
    };
    choice(QCoreApplication::translate("XLibrary", "Find my applications"),
           QCoreApplication::translate("XLibrary", "Review installed applications and choose what to keep."), "search",
           [this] { service.discover(); });
    choice(QCoreApplication::translate("XLibrary", "Add an application manually"),
           QCoreApplication::translate("XLibrary", "Choose a file and customize its name, tags and launch options."),
           "add", [this] { context.actions.value("add_application")->trigger(); });
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
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "A quick tour of XLibrary"));
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
        {"keyboard", QCoreApplication::translate("XLibrary", "Keep XLibrary close"),
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
        const auto size = appearance::scaled(service.settings, 48);
        icon->setPixmap(appearance::icon(topic[0], palette().color(QPalette::WindowText)).pixmap(size, size));
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

void MainWindow::dragEnterEvent(QDragEnterEvent *const event) {
    if (event->mimeData()->hasUrls() && service.is_ready() && !service.is_discovering() && !service.profile_busy() &&
        !service.restart_required()) {
        for (const auto &url : event->mimeData()->urls())
            if (!url.isLocalFile())
                return;
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *const event) {
    QStringList paths;
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    if (!paths.isEmpty()) {
        service.preview_files(paths);
        event->acceptProposedAction();
    }
}
