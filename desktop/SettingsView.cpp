#include <memory>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QProgressBar>
#include <QRegularExpressionValidator>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QUrl>

#include "ApplicationPaths.h"
#include "DesktopService.h"
#include "GithubTokenStore.h"
#include "localization.h"
#include "PlatformIntegration.h"
#include "ProfileView.h"
#include "SelectionButton.h"
#include "SettingsView.h"
#include "UiHelpers.h"
#include "UpdateChecker.h"

namespace {
using namespace ui_helpers;

QWidget *page(const QString &title) {
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(12);
    layout->addWidget(label(title, "subheading"));
    layout->addSpacing(8);
    return widget;
}

void checkbox(QWidget *const page, QSettings &settings, const QString &key, const QString &text,
              const bool default_value) {
    auto *control = new QCheckBox(text);
    control->setProperty("setting_key", key);
    control->setChecked(settings.value(key, default_value).toBool());
    page->layout()->addWidget(control);
}

void choices(QWidget *const page, QSettings &settings, const QString &key, const QString &text,
             const QList<QPair<QString, QVariant>> &values, const QVariant &default_value) {
    auto *control = new QComboBox;
    control->setProperty("setting_key", key);
    for (const auto &value : values)
        control->addItem(value.first, value.second);
    const auto index = control->findData(settings.value(key, default_value));
    control->setCurrentIndex(qMax(0, index));
    field(static_cast<QVBoxLayout *>(page->layout()), text, control);
}

QWidget *general(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("XLibrary", "Application behavior"));
    auto &settings = *context.settings;
    widget->layout()->addWidget(action_button(context.actions.value("welcome")));
    choices(
        widget, settings, "interface/language", QCoreApplication::translate("XLibrary", "Interface language"),
        {{QCoreApplication::translate("XLibrary", "System language"), "system"}, {"English", "en"}, {"Русский", "ru"}},
        "system");
    widget->layout()->addWidget(label(
        QCoreApplication::translate("XLibrary", "Language changes take effect after restarting XLibrary."), "muted"));
    checkbox(widget, settings, "behavior/autostart", QCoreApplication::translate("XLibrary", "Start at login"), false);
    checkbox(widget, settings, "behavior/close_to_tray",
             QCoreApplication::translate("XLibrary", "Minimize to tray on close"), true);
    choices(widget, settings, "behavior/after_launch",
            QCoreApplication::translate("XLibrary", "After launching an application"),
            {{QCoreApplication::translate("XLibrary", "Keep the window open in the background"), "keep"},
             {QCoreApplication::translate("XLibrary", "Minimize the window"), "minimize"}},
            "keep");
    choices(widget, settings, "behavior/relaunch", QCoreApplication::translate("XLibrary", "Launch again"),
            {{QCoreApplication::translate("XLibrary", "Switch to the open window"), "activate"},
             {QCoreApplication::translate("XLibrary", "Open a new instance"), "new"}},
            "activate");
    choices(widget, settings, "behavior/startup", QCoreApplication::translate("XLibrary", "At startup"),
            {{QCoreApplication::translate("XLibrary", "Show the library"), "show"},
             {QCoreApplication::translate("XLibrary", "Start in the tray"), "tray"}},
            "show");
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *appearance_page(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("XLibrary", "Appearance"));
    auto &settings = *context.settings;
    choices(widget, settings, "appearance/theme", QCoreApplication::translate("XLibrary", "Theme"),
            {{QCoreApplication::translate("XLibrary", "Dark"), "dark"},
             {QCoreApplication::translate("XLibrary", "Light"), "light"},
             {QCoreApplication::translate("XLibrary", "System"), "system"}},
            "dark");
#ifdef Q_OS_WIN
    checkbox(widget, settings, "appearance/glass",
             QCoreApplication::translate("XLibrary", "Windows 11 system window material"), false);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("XLibrary",
                                          "Windows 10 uses a standard opaque frame. The list and text remain opaque."),
              "muted"));
#endif
    choices(widget, settings, "appearance/text_scale", QCoreApplication::translate("XLibrary", "Text size"),
            {{"100%", 100}, {"125%", 125}, {"150%", 150}, {"200%", 200}}, 100);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("XLibrary",
                                          "Short, smooth transitions follow available system animation settings."),
              "muted"));
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *hotkeys(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("XLibrary", "Keyboard shortcuts"));
    auto *tabs = new QTabBar;
    tabs->setObjectName("shortcut_tabs");
    tabs->setDrawBase(false);
    tabs->setExpanding(true);
    tabs->setElideMode(Qt::ElideNone);
    tabs->setFocusPolicy(Qt::StrongFocus);
    auto *pages = new QStackedWidget;
    pages->setObjectName("shortcut_pages");
    auto *transition = new PageTransition(pages);
    widget->layout()->addWidget(tabs);
    static_cast<QVBoxLayout *>(widget->layout())->addWidget(transition, 1);
    struct shortcut_group {
        QString title;
        QList<QStringList> values;
    };
    const QList<shortcut_group> groups{
        {QCoreApplication::translate("XLibrary", "Global shortcuts"),
         {{"shortcuts/global", QCoreApplication::translate("XLibrary", "Show the library from any application"),
           desktop_platform::default_global_shortcut()},
          {"shortcuts/overlay", QCoreApplication::translate("XLibrary", "Show quick search from any application"),
           desktop_platform::default_overlay_shortcut()}}},
        {QCoreApplication::translate("XLibrary", "Navigation"),
         {{"shortcuts/search", QCoreApplication::translate("XLibrary", "Search the library"), "Ctrl+K"},
          {"shortcuts/settings", QCoreApplication::translate("XLibrary", "Open settings"), "Ctrl+,"}}},
        {QCoreApplication::translate("XLibrary", "Application actions"),
         {{"shortcuts/add", QCoreApplication::translate("XLibrary", "Add application"), "Ctrl+N"},
          {"shortcuts/launch", QCoreApplication::translate("XLibrary", "Launch application"), "Return"},
          {"shortcuts/new_instance", QCoreApplication::translate("XLibrary", "New instance"), "Ctrl+Return"},
          {"shortcuts/open_folder", QCoreApplication::translate("XLibrary", "Open application folder"), "Alt+Return"},
          {"shortcuts/copy_path", QCoreApplication::translate("XLibrary", "Copy path"), "Ctrl+Shift+C"},
          {"shortcuts/remove", QCoreApplication::translate("XLibrary", "Remove from library"), "Delete"}}}};
    for (const auto &group : groups) {
        tabs->addTab(group.title);
        auto *contents = new QWidget;
        auto *layout = new QVBoxLayout(contents);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        for (const auto &value : group.values) {
            auto *control = new QKeySequenceEdit(QKeySequence::fromString(
                context.settings->value(value[0], value[2]).toString(), QKeySequence::PortableText));
            control->setMaximumSequenceLength(1);
            control->setClearButtonEnabled(true);
            control->setProperty("setting_key", value[0]);
            field(layout, value[1], control);
        }
#ifdef Q_OS_LINUX
        if (pages->count() == 0)
            layout->addWidget(label(
                QCoreApplication::translate(
                    "XLibrary", "Wayland shortcuts request permission through the GlobalShortcuts portal when "
                                "supported. If refused or unavailable, bind XLibrary --overlay in system "
                                "keyboard settings: KDE: Keyboard → Shortcuts; GNOME: Keyboard → Custom "
                                "Shortcuts; Xfce: Keyboard → Application Shortcuts. Use XLibrary for the library."),
                "muted"));
#endif
        layout->addStretch();
        pages->addWidget(contents);
    }
    auto *service = static_cast<DesktopService *>(context.service);
    for (const auto scenario : {false, true}) {
        tabs->addTab(scenario ? QCoreApplication::translate("XLibrary", "Scenarios")
                              : QCoreApplication::translate("XLibrary", "Applications"));
        auto *contents = new QWidget;
        auto *layout = new QVBoxLayout(contents);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        layout->addWidget(
            label(QCoreApplication::translate(
                      "XLibrary",
                      "Assign a global shortcut in the application or scenario editor. An empty shortcut disables it."),
                  "muted"));
        auto *targets = new QTreeWidget;
        targets->setRootIsDecorated(false);
        targets->setHeaderLabels({QCoreApplication::translate("XLibrary", "Name"),
                                  QCoreApplication::translate("XLibrary", "Keyboard shortcuts")});
        targets->header()->setStretchLastSection(false);
        targets->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        targets->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        targets->setMinimumHeight(appearance::scaled(*context.settings, 160));
        targets->setUniformRowHeights(true);
        layout->addWidget(targets, 1);
        auto *edit = button(QCoreApplication::translate("XLibrary", "Edit"), "quiet");
        layout->addWidget(edit, 0, Qt::AlignRight);
        const auto open_editor = [=] {
            if (!targets->currentItem())
                return;
            const auto id = targets->currentItem()->data(0, Qt::UserRole).toLongLong();
            auto edit_context = context;
            edit_context.window = contents->window();
            if (scenario) {
                for (const auto &record : service->catalog().scenarios)
                    if (record.id == id) {
                        edit_context.parameters.insert("scenario", QVariant::fromValue(record));
                        open_dialog("scenario_editor", edit_context);
                        break;
                    }
            } else if (const auto *record = service->model.find(id)) {
                edit_context.parameters.insert("record", QVariant::fromValue(*record));
                open_dialog("application_editor", edit_context);
            }
        };
        QObject::connect(edit, &QPushButton::clicked, contents, open_editor);
        QObject::connect(targets, &QTreeWidget::itemActivated, contents,
                         [=](QTreeWidgetItem *, int) { open_editor(); });
        QObject::connect(targets, &QTreeWidget::itemSelectionChanged, contents,
                         [=] { edit->setEnabled(targets->currentItem() != nullptr); });
        const auto rebuild = [=] {
            const auto selected =
                targets->currentItem() ? targets->currentItem()->data(0, Qt::UserRole).toLongLong() : 0;
            targets->clear();
            const auto add_target = [=](const qint64 id, const QString &name) {
                const auto sequence = service->target_shortcut(scenario, id);
                auto *item = new QTreeWidgetItem(
                    targets, {name, sequence.isEmpty() ? QCoreApplication::translate("XLibrary", "Not assigned")
                                                       : sequence.toString(QKeySequence::NativeText)});
                item->setData(0, Qt::UserRole, id);
                item->setToolTip(0, QStringLiteral("XLibrary ") +
                                        (scenario ? QStringLiteral("--scenario-id ") : QStringLiteral("--launch-id ")) +
                                        QString::number(id));
                if (id == selected)
                    targets->setCurrentItem(item);
            };
            if (scenario) {
                for (const auto &record : service->catalog().scenarios)
                    add_target(record.id, record.name);
            } else {
                for (const auto &record : service->catalog().applications)
                    add_target(record.id, record.name);
            }
            edit->setEnabled(targets->currentItem() != nullptr);
        };
        QObject::connect(service, &DesktopService::catalog_changed, contents, rebuild);
        QObject::connect(service, &DesktopService::target_shortcuts_changed, contents, rebuild);
        rebuild();
#ifdef Q_OS_LINUX
        layout->addWidget(
            label(QCoreApplication::translate("XLibrary", "On Wayland, configure a desktop shortcut for XLibrary "
                                                          "--launch-id <id> or XLibrary --scenario-id <id>."),
                  "muted"));
#endif
        pages->addWidget(contents);
    }
    QObject::connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    QObject::connect(pages, &QStackedWidget::currentChanged, tabs, &QTabBar::setCurrentIndex);
    QObject::connect(pages, &QStackedWidget::currentChanged, transition, [transition] { transition->start(); });
    widget->layout()->addWidget(label(
        QCoreApplication::translate("XLibrary",
                                    "An empty field disables the shortcut. Application actions use the "
                                    "selected library entry. Text editing keys keep their usual behavior "
                                    "in text fields. ↑↓ selects a row. Escape clears the search and is reserved."),
        "muted"));
    return widget;
}

QWidget *library_settings(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("XLibrary", "Application sources"));
#ifdef Q_OS_WIN
    checkbox(widget, *context.settings, "sources/registry",
             QCoreApplication::translate("XLibrary", "Registered Windows applications"), true);
    checkbox(widget, *context.settings, "sources/start_menu",
             QCoreApplication::translate("XLibrary", "Start menu shortcuts"), true);
#else
    checkbox(widget, *context.settings, "sources/registry",
             QCoreApplication::translate("XLibrary", "Linux system applications"), true);
    checkbox(widget, *context.settings, "sources/start_menu",
             QCoreApplication::translate("XLibrary", "Linux user applications"), true);
#endif
    widget->layout()->addWidget(label(
        QCoreApplication::translate(
            "XLibrary", "Applications are checked in the background at every startup. New applications are offered "
                        "for selection. Applications removed from the library are not offered again. "
                        "Changed sources will be used after saving settings."),
        "muted"));
    checkbox(widget, *context.settings, "sources/confirm_removal",
             QCoreApplication::translate("XLibrary", "Ask before removing uninstalled applications from the library"),
             true);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("XLibrary",
                                          "When disabled, confirmed missing applications are removed automatically. "
                                          "Unavailable drives and uncertain targets are kept."),
              "muted"));
    widget->layout()->addWidget(action_button(context.actions.value("discover")));
    auto *clear_exclusions = button(QCoreApplication::translate("XLibrary", "Reset discovery exclusions"));
    widget->layout()->addWidget(clear_exclusions);
    QObject::connect(clear_exclusions, &QPushButton::clicked, service, &DesktopService::clear_discovery_exclusions);
    widget->layout()->addWidget(context.registry->create("discovery_progress", context, widget));
    widget->layout()->addWidget(
        label(QCoreApplication::translate("XLibrary", "Library import and export"), "subheading"));
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary",
                  "JSON saves applications, parameters, favorites, and tags. Review the entries and conflict policy "
                  "before importing. Matching configurations are skipped by default. Applications are never launched "
                  "automatically."),
              "muted"));
    auto *import = button(QCoreApplication::translate("XLibrary", "Import…"));
    auto *export_button = button(QCoreApplication::translate("XLibrary", "Export…"));
    widget->layout()->addWidget(import);
    widget->layout()->addWidget(export_button);
    QObject::connect(import, &QPushButton::clicked, widget, [=] {
        const auto path =
            QFileDialog::getOpenFileName(widget, QCoreApplication::translate("XLibrary", "Import library"), {},
                                         QCoreApplication::translate("XLibrary", "XLibrary catalog (*.json)"));
        if (!path.isEmpty())
            service->import_catalog(path);
    });
    QObject::connect(export_button, &QPushButton::clicked, widget, [=] {
        const auto path = QFileDialog::getSaveFileName(
            widget, QCoreApplication::translate("XLibrary", "Export library"), "xlibrary.json",
            QCoreApplication::translate("XLibrary", "XLibrary catalog (*.json)"));
        if (!path.isEmpty())
            service->export_catalog(path);
    });
    auto *clear = button(QCoreApplication::translate("XLibrary", "Clear launch history"));
    widget->layout()->addWidget(clear);
    QObject::connect(clear, &QPushButton::clicked, widget, [=] {
        if (confirm(widget, QCoreApplication::translate("XLibrary", "Launch history"),
                    QCoreApplication::translate(
                        "XLibrary", "Clear the most recent launch times? Applications and tags will be kept."),
                    QCoreApplication::translate("XLibrary", "Clear")))
            service->clear_history();
    });
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *storage_page(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("XLibrary", "Local data"));
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary",
                  "Your library, tags, scenarios and settings are stored on this computer. XLibrary does not upload "
                  "your catalog. Update checks contact GitHub; your saved access token is kept in the system "
                  "credential store."),
              "muted"));
    const auto add_path = [=](const QString &title, const QString &path) {
        widget->layout()->addWidget(label(title, "subheading"));
        auto *value = label(QDir::toNativeSeparators(path), "muted");
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        widget->layout()->addWidget(value);
    };
    add_path(QCoreApplication::translate("XLibrary", "Settings file"), context.settings->fileName());
    auto *database = label("", "muted");
    database->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    widget->layout()->addWidget(label(QCoreApplication::translate("XLibrary", "Catalog database"), "subheading"));
    widget->layout()->addWidget(database);
    const auto update_path = [=] { database->setText(QDir::toNativeSeparators(service->library_path())); };
    QObject::connect(service, &DesktopService::catalog_changed, widget, update_path);
    update_path();
    widget->layout()->addWidget(label(application_paths::portable_mode()
                                          ? QCoreApplication::translate("XLibrary", "Current mode: portable")
                                          : QCoreApplication::translate("XLibrary", "Current mode: installed"),
                                      "subheading"));
    widget->layout()->addWidget(label(
        QCoreApplication::translate(
            "XLibrary",
            "Installed mode uses your operating system's user data folders. A portable.flag next to the executable "
            "selects the data folder beside it. An AppImage uses <AppImage>.data beside the image. Moving the "
            "executable "
            "alone does not transfer installed-mode data; use a complete profile backup to move your library."),
        "muted"));
    auto *folder = button(QCoreApplication::translate("XLibrary", "Open profile folder"));
    widget->layout()->addWidget(folder);
    QObject::connect(folder, &QPushButton::clicked, service, &DesktopService::open_profile_folder);
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *plugins(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("XLibrary", "Interface modules"));
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary",
                  "Native plugins can change interface areas and appearance. Enable only trusted modules; they run "
                  "inside XLibrary. Changes take effect after restart."),
              "muted"));
    auto available = context.parameters.value("available_plugins").toStringList();
    const auto enabled = context.settings->value("plugins/enabled").toStringList();
    if (service->safe_mode()) {
        widget->layout()->addWidget(label(
            QCoreApplication::translate(
                "XLibrary", "Safe mode skips native modules. Saved enabled modules are listed below so you can disable "
                            "a faulty module before restarting."),
            "muted"));
        for (const auto &id : enabled)
            if (!available.contains(id))
                available.append(id);
    }
    const auto errors = context.parameters.value("plugin_errors").toMap();
    for (const auto &id : available) {
        auto *control = new QCheckBox(id);
        control->setProperty("plugin_id", id);
        control->setChecked(enabled.contains(id));
        widget->layout()->addWidget(control);
        const auto error = errors.value(id).toString();
        if (!error.isEmpty()) {
            control->setToolTip(error);
            // Keep an enabled failed module selectable so it can be disabled.
            if (!control->isChecked())
                control->setEnabled(false);
            widget->layout()->addWidget(label(error, "muted"));
        }
    }
    if (available.isEmpty() && !service->safe_mode())
        widget->layout()->addWidget(
            label(QCoreApplication::translate("XLibrary",
                                              "No modules installed. Folder: plugins next to the XLibrary executable."),
                  "muted"));
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *help_page(const ui_context &, QWidget *) {
    auto *widget = page(QCoreApplication::translate("XLibrary", "Quick help"));
    const QList<QPair<QString, QString>> topics{
        {QCoreApplication::translate("XLibrary", "Search"),
         QCoreApplication::translate("XLibrary",
                                     "Type an application name, alias or filename. Use #work to require a tag and "
                                     "-#games to exclude one; combine them with text. "
                                     "Clear filters to search the whole library. Quick search also accepts file paths "
                                     "and calculations such as (2 + 3) * 4; Enter copies the result.")},
        {QCoreApplication::translate("XLibrary", "Tags"),
         QCoreApplication::translate("XLibrary", "Create tags in the sidebar or application editor. A tag has up to 64 "
                                                 "characters without spaces, commas or #. "
                                                 "Select several applications with Ctrl or Shift to edit their tags "
                                                 "together. Removing a tag keeps the applications.")},
        {QCoreApplication::translate("XLibrary", "Scenarios"),
         QCoreApplication::translate("XLibrary", "Create a scenario from the Scenarios section. Add applications in "
                                                 "launch order, set each delay and choose a launch profile. "
                                                 "Disabled steps are skipped. Fix the listed problems before running. "
                                                 "Success means the launch request was accepted, "
                                                 "not that the application finished loading. Stop skips remaining "
                                                 "steps and keeps already opened applications running.")},
        {QCoreApplication::translate("XLibrary", "Keyboard shortcuts"),
         QCoreApplication::translate(
             "XLibrary",
             "Open Settings → Keyboard shortcuts to see or change the current bindings. Clear a binding to disable it. "
             "Assign application and scenario shortcuts in their editors. A shortcut already used by XLibrary or "
             "another application "
             "may be refused. On Wayland, system permission or a desktop shortcut may be required.")}};
    for (const auto &topic : topics) {
        widget->layout()->addWidget(label(topic.first, "subheading"));
        widget->layout()->addWidget(label(topic.second, "muted"));
    }
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *about(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("XLibrary", "About"));
    auto *card = new QFrame;
    card->setProperty("card", true);
    auto *card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(24, 24, 24, 24);
    card_layout->setSpacing(12);
    auto *identity = new QHBoxLayout;
    identity->setSpacing(16);
    auto *logo = new QLabel;
    const auto logo_size = appearance::scaled(*context.settings, 64);
    logo->setPixmap(appearance::logo().pixmap(logo_size, logo_size));
    identity->addWidget(logo, 0, Qt::AlignTop);
    auto *title = new QVBoxLayout;
    title->addWidget(label("XLibrary", "heading"));
    auto *version =
        label(QCoreApplication::translate("XLibrary", "Version ") + QCoreApplication::applicationVersion(), "muted");
    version->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    title->addWidget(version);
    identity->addLayout(title, 1);
    card_layout->addLayout(identity);
    card_layout->addWidget(
        label(QCoreApplication::translate("XLibrary", "Application library for Windows and Linux"), "subheading"));
    card_layout->addWidget(label(
        QCoreApplication::translate("XLibrary", "Applications, tags, and scenarios in one place. The catalog is stored "
                                                "locally; no internet connection is required."),
        "muted"));
    auto *github = button(QCoreApplication::translate("XLibrary", "Open GitHub ↗"));
    auto *issues = button(QCoreApplication::translate("XLibrary", "Report an issue ↗"));
    card_layout->addWidget(github);
    card_layout->addWidget(issues);
    QObject::connect(github, &QPushButton::clicked, widget,
                     [] { desktop_platform::open_url(QUrl("https://github.com/lappland-open/XLibrary")); });
    QObject::connect(issues, &QPushButton::clicked, widget,
                     [] { desktop_platform::open_url(QUrl("https://github.com/lappland-open/XLibrary/issues")); });
    widget->layout()->addWidget(card);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("XLibrary", "Technologies and licenses"), "subheading"));
    widget->layout()->addWidget(label(
        QString(QCoreApplication::translate("XLibrary", "Qt %1 · C++23 · SQLite · QtKeychain\nXLibrary — "
                                                        "MIT\nThird-party licenses are included with the application."))
            .arg(QString::fromLatin1(qVersion())),
        "muted"));
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *update_page(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("XLibrary", "Update"));
    auto *card = new QFrame;
    card->setProperty("card", true);
    auto *card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(24, 24, 24, 24);
    card_layout->setSpacing(12);
    // The checker and its cache outlive the settings window.
    auto *updates = &static_cast<DesktopService *>(context.service)->updates;
    auto request_started = std::make_shared<bool>(false);
    QObject::connect(widget, &QObject::destroyed, updates, [updates, request_started] {
        // Closing settings does not cancel the automatic startup check.
        if (*request_started)
            updates->cancel();
    });
    auto *channel = new QComboBox;
    channel->addItem(QCoreApplication::translate("XLibrary", "Stable"), "stable");
    channel->addItem(QCoreApplication::translate("XLibrary", "Preview"), "preview");
    channel->setCurrentIndex(channel->findData(updates->channel()));
    field(card_layout, QCoreApplication::translate("XLibrary", "Update channel"), channel);
    card_layout->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary", "Stable includes published stable releases. Preview also includes pre-releases. "
                              "Changing the channel takes effect immediately and cancels any current update request."),
              "muted"));
    auto *tokens = new GithubTokenStore(*context.settings, context.service);
    // libsecret completes requests asynchronously; closing the window must not destroy an active job.
    QObject::connect(widget, &QObject::destroyed, tokens, [tokens] {
        if (!tokens->is_busy()) {
            tokens->deleteLater();
        } else {
            QObject::connect(tokens, &GithubTokenStore::busy_changed, tokens, [tokens] {
                if (!tokens->is_busy())
                    tokens->deleteLater();
            });
        }
    });
    auto *token = new QLineEdit;
    token->setEchoMode(QLineEdit::Password);
    token->setMaxLength(1024);
    token->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9_]*"), token));
    token->setPlaceholderText(QCoreApplication::translate("XLibrary", "Paste a GitHub Personal Access Token"));
    field(card_layout, QCoreApplication::translate("XLibrary", "Personal GitHub token (optional)"), token);
    card_layout->addWidget(label(
        QCoreApplication::translate("XLibrary", "For public releases, use a token without additional permissions. "
                                                "It is sent only to the GitHub API."),
        "muted"));
    auto *token_buttons = new QHBoxLayout;
    auto *save_token = button(QCoreApplication::translate("XLibrary", "Save token"));
    auto *remove_token = button(QCoreApplication::translate("XLibrary", "Delete token"));
    // Pressing Enter in the token field must not save the token instead of the regular settings.
    save_token->setAutoDefault(false);
    remove_token->setAutoDefault(false);
    token_buttons->addWidget(save_token);
    token_buttons->addWidget(remove_token);
    card_layout->addLayout(token_buttons);
    auto *token_status = label(
        tokens->has_token()
            ? QCoreApplication::translate("XLibrary", "A personal token is saved in the system secret store.")
            : QCoreApplication::translate("XLibrary", "No token saved. Update checks use unauthenticated requests."),
        "muted");
    card_layout->addWidget(token_status);
    card_layout->addWidget(label(
        QCoreApplication::translate("XLibrary", "Save token and Delete token take effect immediately. "
                                                "The token is not stored in the settings file or library export."),
        "muted"));
    auto *check_update = button(QCoreApplication::translate("XLibrary", "Check for updates"));
    auto *update_status =
        label(QCoreApplication::translate(
                  "XLibrary", "Checks GitHub Releases at startup and on request for the selected channel. "
                              "Results are cached for 60 seconds without a token or 5 seconds with a token."),
              "muted");
    auto *update_progress = new QProgressBar;
    update_progress->setAccessibleName(QCoreApplication::translate("XLibrary", "Update download progress"));
    update_progress->hide();
    auto *cancel_update = button(QCoreApplication::translate("XLibrary", "Cancel"));
    cancel_update->hide();
    auto *release_button = button(QCoreApplication::translate("XLibrary", "Open release on GitHub ↗"));
    release_button->hide();
    auto *install_update = button(QCoreApplication::translate("XLibrary", "Update now"));
    install_update->setAutoDefault(false);
    install_update->hide();
    card_layout->addWidget(check_update);
    card_layout->addWidget(update_status);
    card_layout->addWidget(update_progress);
    card_layout->addWidget(cancel_update);
    card_layout->addWidget(install_update);
    card_layout->addWidget(release_button);
    QObject::connect(channel, &QComboBox::currentIndexChanged, widget,
                     [=] { updates->set_channel(channel->currentData().toString()); });
    QObject::connect(updates, &UpdateChecker::channel_changed, widget, [=] {
        channel->setCurrentIndex(channel->findData(updates->channel()));
        release_button->hide();
        install_update->hide();
        update_progress->hide();
        update_status->setText(QCoreApplication::translate("XLibrary", "Channel changed. Check for updates."));
    });
    auto open_release = [=] {
        if (!desktop_platform::open_url(updates->available_release_url()))
            update_status->setText(QCoreApplication::translate("XLibrary", "Could not open the release in a browser."));
    };
    QObject::connect(release_button, &QPushButton::clicked, widget, open_release);
    auto refresh_token_controls = [=] {
        const bool idle =
            !updates->is_busy() && !tokens->is_busy() && !service->profile_busy() && !service->restart_required();
        channel->setEnabled(!service->profile_busy() && !service->restart_required() && !tokens->is_busy());
        token->setEnabled(idle);
        save_token->setEnabled(idle && !token->text().isEmpty() && token->hasAcceptableInput());
        remove_token->setEnabled(idle && tokens->has_token());
        check_update->setEnabled(idle);
        install_update->setEnabled(idle);
    };
    QObject::connect(token, &QLineEdit::textChanged, widget, refresh_token_controls);
    QObject::connect(tokens, &GithubTokenStore::busy_changed, widget, refresh_token_controls);
    QObject::connect(updates, &UpdateChecker::busy_changed, widget, refresh_token_controls);
    QObject::connect(service, &DesktopService::profile_state_changed, widget, refresh_token_controls);
    refresh_token_controls();
    QObject::connect(save_token, &QPushButton::clicked, widget, [=] {
        token_status->setText(QCoreApplication::translate("XLibrary", "Saving token…"));
        tokens->save(token->text());
    });
    QObject::connect(remove_token, &QPushButton::clicked, widget, [=] {
        token_status->setText(QCoreApplication::translate("XLibrary", "Deleting token…"));
        tokens->remove();
    });
    QObject::connect(tokens, &GithubTokenStore::changed, widget, [=](const bool stored) {
        token->clear();
        token_status->setText(
            stored ? QCoreApplication::translate("XLibrary", "A personal token is saved in the system secret store.")
                   : QCoreApplication::translate("XLibrary",
                                                 "No token saved. Update checks use unauthenticated requests."));
    });
    QObject::connect(tokens, &GithubTokenStore::failed, widget, [=](const QString &message) {
        token_status->setText(message);
        update_progress->hide();
        update_status->setText(message);
    });
    QObject::connect(tokens, &GithubTokenStore::read_finished, widget, [=](const QString &value) {
        const bool was_busy = updates->is_busy();
        updates->check(value);
        // Token retrieval may finish after the background request has started.
        *request_started = !was_busy && updates->is_busy();
    });
    QObject::connect(updates, &UpdateChecker::busy_changed, widget, [=] {
        cancel_update->setVisible(updates->is_busy());
        release_button->setEnabled(!updates->is_busy());
        if (!updates->is_busy()) {
            *request_started = false;
            update_progress->hide();
        }
    });
    QObject::connect(check_update, &QPushButton::clicked, widget, [=] {
        release_button->hide();
        install_update->hide();
        update_status->setText(QCoreApplication::translate("XLibrary", "Checking for updates…"));
        update_progress->setRange(0, 0);
        update_progress->show();
        tokens->read();
    });
    QObject::connect(cancel_update, &QPushButton::clicked, widget, [=] {
        updates->cancel();
        update_status->setText(QCoreApplication::translate("XLibrary", "Update cancelled."));
    });
    QObject::connect(updates, &UpdateChecker::failed, widget, [=](const QString &message) {
        update_progress->hide();
        update_status->setText(message);
    });
    QObject::connect(updates, &UpdateChecker::operation_changed, widget, [=](const operation_result &result) {
        if (result.state == operation_state::cancelled) {
            update_progress->hide();
            update_status->setText(QCoreApplication::translate("XLibrary", "Update cancelled."));
        }
    });
    QObject::connect(updates, &UpdateChecker::up_to_date, widget, [=](const bool releases_exist) {
        update_status->setText(
            releases_exist
                ? QCoreApplication::translate("XLibrary", "You are using the latest available version.")
                : QCoreApplication::translate("XLibrary", "No releases with a supported version were found."));
    });
    QObject::connect(
        updates, &UpdateChecker::download_progress, widget, [=](const qint64 received, const qint64 total) {
            update_progress->setRange(0, 100);
            update_progress->setValue(static_cast<int>(received * 100 / total));
            update_progress->show();
            update_status->setText(QCoreApplication::translate("XLibrary", "Downloading update… %1 / %2 MiB")
                                       .arg(received / (1024.0 * 1024.0), 0, 'f', 1)
                                       .arg(total / (1024.0 * 1024.0), 0, 'f', 1));
        });
    auto show_available_update = [=] {
        release_button->show();
        install_update->setText(
            QCoreApplication::translate("XLibrary", updates->can_install() ? "Update now" : "Open release"));
        install_update->show();
        update_status->setText(QCoreApplication::translate("XLibrary", "New version available: %1 (%2 channel)")
                                   .arg(updates->available_version(), channel->currentText()));
    };
    QObject::connect(updates, &UpdateChecker::update_available, widget, show_available_update);
    if (!updates->available_version().isEmpty())
        show_available_update();
    if (updates->is_busy()) {
        update_status->setText(QCoreApplication::translate("XLibrary", "Checking for updates…"));
        update_progress->setRange(0, 0);
        update_progress->show();
        cancel_update->show();
        release_button->setEnabled(false);
    }
    QObject::connect(install_update, &QPushButton::clicked, widget, [=] {
        if (updates->is_busy() || tokens->is_busy() || updates->available_version().isEmpty())
            return;
        if (updates->can_install()) {
            if (confirm(widget, QCoreApplication::translate("XLibrary", "XLibrary update"),
                        QCoreApplication::translate(
                            "XLibrary", "A new version is available: %1 (current: %2). Update now?\n\n"
                                        "XLibrary will download and verify the installer, then close. "
                                        "Complete the update in the installation wizard. Your library and settings "
                                        "will be kept. Save any unsaved edits before continuing.")
                            .arg(updates->available_version(), QCoreApplication::applicationVersion()),
                        QCoreApplication::translate("XLibrary", "Update now"))) {
                *request_started = true;
                updates->download();
            }
        } else if (confirm(widget, QCoreApplication::translate("XLibrary", "XLibrary update"),
                           QCoreApplication::translate(
                               "XLibrary",
                               "A new version is available: %1 (current: %2). Open the release on GitHub?\n\n"
                               "Automatic installation is unavailable for this copy or release. "
                               "Follow the release instructions and keep your existing profile.")
                               .arg(updates->available_version(), QCoreApplication::applicationVersion()),
                           QCoreApplication::translate("XLibrary", "Open release"))) {
            open_release();
        }
    });
    QObject::connect(updates, &UpdateChecker::installer_ready, widget, [=] {
        QString error;
        if (updates->launch_installer(error)) {
            context.actions.value("quit")->trigger();
        } else {
            updates->cancel();
            update_status->setText(error);
        }
    });
    widget->layout()->addWidget(card);
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QDialog *settings_dialog(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *hotkey = static_cast<GlobalHotkey *>(context.parameters.value("hotkey").value<QObject *>());
    auto *overlay_hotkey = static_cast<GlobalHotkey *>(context.parameters.value("overlay_hotkey").value<QObject *>());
    auto *dialog = new QDialog(parent);
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "XLibrary settings"));
    prepare_dialog(dialog, context, {960, 700}, {640, 420});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);
    root->addWidget(label(QCoreApplication::translate("XLibrary", "Settings"), "heading"));
    root->addWidget(
        label(QCoreApplication::translate("XLibrary", "XLibrary appearance, behavior, and features"), "muted"));
    auto *body = new QHBoxLayout;
    body->setSpacing(16);
    auto *categories = new QButtonGroup(dialog);
    auto *navigation = new QWidget;
    navigation->setObjectName("settings_categories");
    auto *navigation_layout = new QVBoxLayout(navigation);
    navigation_layout->setContentsMargins(0, 4, 0, 4);
    navigation_layout->setSpacing(8);
    auto *pages = new QStackedWidget;
    const QList<QPair<QString, QString>> sections{
        {QCoreApplication::translate("XLibrary", "General"), "settings_general"},
        {QCoreApplication::translate("XLibrary", "Appearance settings"), "settings_appearance"},
        {QCoreApplication::translate("XLibrary", "Keyboard shortcuts"), "settings_hotkeys"},
        {QCoreApplication::translate("XLibrary", "Library"), "settings_library"},
        {QCoreApplication::translate("XLibrary", "Profile"), "settings_profile"},
        {QCoreApplication::translate("XLibrary", "Local data"), "settings_storage"},
        {QCoreApplication::translate("XLibrary", "Plugins"), "settings_plugins"},
        {QCoreApplication::translate("XLibrary", "Update"), "settings_update"},
        {QCoreApplication::translate("XLibrary", "Quick help"), "settings_help"},
        {QCoreApplication::translate("XLibrary", "About"), "settings_about"}};
    for (const auto &section : sections) {
        const auto index = pages->count();
        auto *item = new SelectionButton(section.first);
        item->setObjectName(section.second + "_category");
        categories->addButton(item, index);
        navigation_layout->addWidget(item);
        auto *holder = new QWidget;
        auto *page_layout = new QHBoxLayout(holder);
        page_layout->setContentsMargins(0, 0, 0, 0);
        auto *contents = context.registry->create(section.second, context, holder);
        page_layout->addWidget(contents, 1);
        pages->addWidget(scroll(holder));
    }
    navigation_layout->addStretch();
    auto *navigation_scroll = scroll(navigation);
    navigation_scroll->setMinimumWidth(appearance::scaled(*context.settings, 184));
    navigation_scroll->setMaximumWidth(appearance::scaled(*context.settings, 220));
    auto *transition = new PageTransition(pages);
    body->addWidget(navigation_scroll);
    auto *surface = new QFrame;
    surface->setObjectName("settings_surface");
    surface->setProperty("card", true);
    auto *surface_layout = new QVBoxLayout(surface);
    surface_layout->setContentsMargins(12, 12, 12, 12);
    surface_layout->addWidget(transition);
    body->addWidget(surface, 1);
    root->addLayout(body, 1);
    QObject::connect(categories, &QButtonGroup::idClicked, pages, [=](const int index) {
        if (pages->currentIndex() == index)
            return;
        pages->setCurrentIndex(index);
        transition->start();
    });
    categories->button(0)->setChecked(true);
    const auto initial_section = context.parameters.value("settings_section").toString();
    for (qsizetype index = 0; index < sections.size(); ++index)
        if (sections[index].second == initial_section)
            categories->button(static_cast<int>(index))->click();
    auto *error = label("", "error");
    error->hide();
    root->addWidget(error);
    auto show_error = [=](const QString &text) {
        error->setText(text);
        error->setVisible(!text.isEmpty());
    };
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("XLibrary", "Save"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    root->addWidget(buttons);
    const auto update_save = [=] {
        buttons->button(QDialogButtonBox::Save)->setEnabled(!service->profile_busy() && !service->restart_required());
    };
    QObject::connect(service, &DesktopService::profile_state_changed, dialog, update_save);
    update_save();
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        if (service->profile_busy() || service->restart_required()) {
            service->notify(QCoreApplication::translate(
                "XLibrary", "Settings cannot be saved while a profile operation is running or a restart is required."));
            return;
        }
        QVariantMap values;
        for (auto *control : dialog->findChildren<QWidget *>()) {
            const auto key = control->property("setting_key").toString();
            if (key.isEmpty())
                continue;
            if (auto *box = qobject_cast<QCheckBox *>(control))
                values.insert(key, box->isChecked());
            if (auto *combo = qobject_cast<QComboBox *>(control))
                values.insert(key, combo->currentData());
            if (auto *edit = qobject_cast<QKeySequenceEdit *>(control)) {
                const auto value = edit->keySequence().toString(QKeySequence::PortableText);
                values.insert(key, value);
            }
        }
        QString shortcut_error;
        if (!service->validate_shortcuts(values, shortcut_error)) {
            show_error(shortcut_error);
            categories->button(2)->click();
            return;
        }
        QStringList enabled;
        for (auto *box : dialog->findChildren<QCheckBox *>()) {
            const auto id = box->property("plugin_id").toString();
            if (!id.isEmpty() && box->isChecked())
                enabled.append(id);
        }
        values.insert("plugins/enabled", enabled);
        const auto previous_global = QKeySequence::fromString(
            service->settings.value("shortcuts/global", desktop_platform::default_global_shortcut()).toString(),
            QKeySequence::PortableText);
        const auto previous_overlay = QKeySequence::fromString(
            service->settings.value("shortcuts/overlay", desktop_platform::default_overlay_shortcut()).toString(),
            QKeySequence::PortableText);
        const auto restore_hotkeys = [=] {
            QString ignored;
            if (hotkey)
                hotkey->set_sequence({}, ignored);
            if (overlay_hotkey)
                overlay_hotkey->set_sequence({}, ignored);
            if (hotkey)
                hotkey->set_sequence(previous_global, ignored);
            if (overlay_hotkey)
                overlay_hotkey->set_sequence(previous_overlay, ignored);
        };
        QString failure;
        const bool hotkeys_changed =
            previous_global !=
                QKeySequence::fromString(values.value("shortcuts/global").toString(), QKeySequence::PortableText) ||
            previous_overlay !=
                QKeySequence::fromString(values.value("shortcuts/overlay").toString(), QKeySequence::PortableText);
        if (hotkeys_changed) {
            // Release the pair together so the user can exchange their assignments.
            if (hotkey)
                hotkey->set_sequence({}, failure);
            if (overlay_hotkey)
                overlay_hotkey->set_sequence({}, failure);
        }
        if (hotkeys_changed &&
            ((hotkey && !hotkey->set_sequence(QKeySequence::fromString(values.value("shortcuts/global").toString(),
                                                                       QKeySequence::PortableText),
                                              failure)) ||
             (overlay_hotkey &&
              !overlay_hotkey->set_sequence(
                  QKeySequence::fromString(values.value("shortcuts/overlay").toString(), QKeySequence::PortableText),
                  failure)))) {
            restore_hotkeys();
            show_error(failure);
            categories->button(2)->click();
            if (auto *shortcut_pages = dialog->findChild<QStackedWidget *>("shortcut_pages"))
                shortcut_pages->setCurrentIndex(0);
            return;
        }
        const bool old_autostart = service->settings.value("behavior/autostart", false).toBool();
        const bool new_autostart = values.value("behavior/autostart").toBool();
        if (old_autostart != new_autostart && !desktop_platform::set_autostart(new_autostart, failure)) {
            if (hotkeys_changed)
                restore_hotkeys();
            show_error(failure);
            return;
        }
        QVariantMap previous;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            previous.insert(it.key(), service->settings.value(it.key()));
            service->settings.setValue(it.key(), it.value());
        }
        service->settings.sync();
        if (service->settings.status() != QSettings::NoError) {
            for (auto it = previous.cbegin(); it != previous.cend(); ++it)
                service->settings.setValue(it.key(), it.value());
            if (hotkeys_changed)
                restore_hotkeys();
            if (old_autostart != new_autostart) {
                QString ignored;
                desktop_platform::set_autostart(old_autostart, ignored);
            }
            show_error(QCoreApplication::translate("XLibrary", "Could not save the settings file."));
            return;
        }
        emit service->appearance_changed();
        dialog->accept();
    });
    return dialog;
}
}

void register_settings_views(UiRegistry &registry) {
    registry.add_default("settings", settings_dialog);
    registry.add_default("settings_general", general);
    registry.add_default("settings_appearance", appearance_page);
    registry.add_default("settings_hotkeys", hotkeys);
    registry.add_default("settings_library", library_settings);
    registry.add_default("settings_profile", profile_view);
    registry.add_default("settings_storage", storage_page);
    registry.add_default("settings_plugins", plugins);
    registry.add_default("settings_update", update_page);
    registry.add_default("settings_about", about);
    registry.add_default("settings_help", help_page);
}
