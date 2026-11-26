#include <memory>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpressionValidator>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStylePainter>
#include <QSysInfo>
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

class OverlayRowsSlider final : public QSlider {
    WidgetMotion squeeze{this, 120};

protected:
    void paintEvent(QPaintEvent *event) override {
        const auto scale = squeeze.currentValue().isValid() ? squeeze.currentValue().toReal() : 1.0;
        if (qFuzzyCompare(scale, 1.0)) {
            QSlider::paintEvent(event);
            return;
        }
        QStylePainter painter(this);
        QStyleOptionSlider option;
        initStyleOption(&option);
        const auto handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        option.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderTickmarks;
        painter.drawComplexControl(QStyle::CC_Slider, option);
        option.subControls = QStyle::SC_SliderHandle;
        const auto center = QRectF(handle).center();
        painter.translate(center);
        painter.scale(1.0, scale);
        painter.translate(-center);
        painter.drawComplexControl(QStyle::CC_Slider, option);
    }

public:
    OverlayRowsSlider() : QSlider(Qt::Horizontal) {
        setRange(3, 10);
        setSingleStep(1);
        setPageStep(1);
        setTickInterval(maximum() - minimum());
        setTickPosition(QSlider::TicksBelow);
        connect(&squeeze, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
        const auto animate = [this](qreal target) {
            squeeze.move(squeeze.currentValue().isValid() ? squeeze.currentValue().toReal() : 1.0, target);
        };
        connect(this, &QSlider::sliderPressed, this, [animate] { animate(0.9); });
        connect(this, &QSlider::sliderReleased, this, [animate] { animate(1.0); });
    }
};

QWidget *general(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Application behavior"));
    auto &settings = *context.settings;
    widget->layout()->addWidget(action_button(context.actions.value("welcome")));
    choices(
        widget, settings, "interface/language", QCoreApplication::translate("Fora.Deck", "Interface language"),
        {{QCoreApplication::translate("Fora.Deck", "System language"), "system"}, {"English", "en"}, {"Русский", "ru"}},
        "system");
    widget->layout()->addWidget(label(
        QCoreApplication::translate("Fora.Deck", "Language changes take effect after restarting Fora.Deck."), "muted"));
    checkbox(widget, settings, "behavior/autostart", QCoreApplication::translate("Fora.Deck", "Start at login"), false);
    checkbox(widget, settings, "behavior/close_to_tray",
             QCoreApplication::translate("Fora.Deck", "Minimize to tray on close"), true);
    choices(widget, settings, "behavior/after_launch",
            QCoreApplication::translate("Fora.Deck", "After launching an application"),
            {{QCoreApplication::translate("Fora.Deck", "Keep the window open in the background"), "keep"},
             {QCoreApplication::translate("Fora.Deck", "Minimize the window"), "minimize"}},
            "keep");
    choices(widget, settings, "behavior/relaunch", QCoreApplication::translate("Fora.Deck", "Launch again"),
            {{QCoreApplication::translate("Fora.Deck", "Switch to the open window"), "activate"},
             {QCoreApplication::translate("Fora.Deck", "Open a new instance"), "new"}},
            "activate");
    choices(widget, settings, "behavior/startup", QCoreApplication::translate("Fora.Deck", "At startup"),
            {{QCoreApplication::translate("Fora.Deck", "Show the library"), "show"},
             {QCoreApplication::translate("Fora.Deck", "Start in the tray"), "tray"}},
            "show");
    widget->layout()->addWidget(label(QCoreApplication::translate("Fora.Deck", "Running applications"), "subheading"));
    checkbox(widget, settings, "monitoring/memory", QCoreApplication::translate("Fora.Deck", "Show memory usage"),
             true);
    checkbox(widget, settings, "monitoring/cpu", QCoreApplication::translate("Fora.Deck", "Show CPU usage"), true);
    auto *reset = button(QCoreApplication::translate("Fora.Deck", "Reset settings…"), "danger");
    widget->layout()->addWidget(reset);
    QObject::connect(reset, &QPushButton::clicked, widget, [=] {
        auto *service = static_cast<DesktopService *>(context.service);
        if (!confirm(widget, QCoreApplication::translate("Fora.Deck", "Reset settings"),
                     QCoreApplication::translate("Fora.Deck",
                                                 "Restore default interface, shortcuts, sources and behavior settings? "
                                                 "Applications, tags, scenarios, history and saved credentials will be "
                                                 "kept. Fora.Deck will need a restart."),
                     QCoreApplication::translate("Fora.Deck", "Reset")))
            return;
        if (!service->reset_settings())
            return;
        if (confirm(widget, QCoreApplication::translate("Fora.Deck", "Restart Fora.Deck"),
                    QCoreApplication::translate("Fora.Deck", "Settings were reset. Restart Fora.Deck now?"),
                    QCoreApplication::translate("Fora.Deck", "Restart")))
            service->restart_application();
        if (auto *dialog = qobject_cast<QDialog *>(widget->window()))
            dialog->reject();
    });
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *appearance_page(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Appearance"));
    auto &settings = *context.settings;
    choices(widget, settings, "appearance/theme", QCoreApplication::translate("Fora.Deck", "Theme"),
            {{QCoreApplication::translate("Fora.Deck", "Dark"), "dark"},
             {QCoreApplication::translate("Fora.Deck", "Light"), "light"},
             {QCoreApplication::translate("Fora.Deck", "System"), "system"}},
            "dark");
#ifdef Q_OS_WIN
    checkbox(widget, settings, "appearance/glass",
             QCoreApplication::translate("Fora.Deck", "Windows 11 system window material"), false);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck",
                                          "Windows 10 uses a standard opaque frame. The list and text remain opaque."),
              "muted"));
#endif
    choices(widget, settings, "appearance/text_scale", QCoreApplication::translate("Fora.Deck", "Text size"),
            {{"100%", 100}, {"125%", 125}, {"150%", 150}, {"200%", 200}}, 100);
    auto *slider = new OverlayRowsSlider;
    slider->setObjectName("overlay_rows");
    slider->setProperty("setting_key", "search/overlay_rows");
    slider->setValue(qBound(slider->minimum(), settings.value("search/overlay_rows", 5).toInt(), slider->maximum()));
    auto *number = label(QString::number(slider->value()));
    number->setAlignment(Qt::AlignCenter);
    number->setWordWrap(false);
    number->setMinimumWidth(number->fontMetrics().horizontalAdvance("10") + appearance::scaled(settings, 12));
    auto *row = new QWidget;
    auto *row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->addWidget(slider, 1);
    row_layout->addWidget(number);
    QObject::connect(slider, &QSlider::valueChanged, number,
                     [number](int value) { number->setText(QString::number(value)); });
    const auto caption = QCoreApplication::translate("Fora.Deck", "Visible results in quick search");
    auto *buddy = label(caption, "muted");
    buddy->setBuddy(slider);
    slider->setAccessibleName(caption);
    widget->layout()->addWidget(buddy);
    widget->layout()->addWidget(row);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck",
                                          "Scroll to see more results. The height is limited by the screen size."),
              "muted"));
    checkbox(widget, settings, "appearance/animations",
             QCoreApplication::translate("Fora.Deck", "Enable interface animations"), true);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck",
                                          "Short, smooth transitions follow available system animation settings."),
              "muted"));
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *hotkeys(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Keyboard shortcuts"));
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
        {QCoreApplication::translate("Fora.Deck", "Global shortcuts"),
         {{"shortcuts/global", QCoreApplication::translate("Fora.Deck", "Show the library from any application"),
           desktop_platform::default_global_shortcut()},
          {"shortcuts/overlay", QCoreApplication::translate("Fora.Deck", "Show quick search from any application"),
           desktop_platform::default_overlay_shortcut()}}},
        {QCoreApplication::translate("Fora.Deck", "Navigation"),
         {{"shortcuts/search", QCoreApplication::translate("Fora.Deck", "Search the library"), "Ctrl+K"},
          {"shortcuts/settings", QCoreApplication::translate("Fora.Deck", "Open settings"), "Ctrl+,"}}},
        {QCoreApplication::translate("Fora.Deck", "Application actions"),
         {{"shortcuts/add", QCoreApplication::translate("Fora.Deck", "Add"), "Ctrl+N"},
          {"shortcuts/launch", QCoreApplication::translate("Fora.Deck", "Launch application"), "Return"},
          {"shortcuts/new_instance", QCoreApplication::translate("Fora.Deck", "New instance"), "Ctrl+Return"},
          {"shortcuts/open_folder", QCoreApplication::translate("Fora.Deck", "Open application folder"), "Alt+Return"},
          {"shortcuts/copy_path", QCoreApplication::translate("Fora.Deck", "Copy path"), "Ctrl+Shift+C"},
          {"shortcuts/remove", QCoreApplication::translate("Fora.Deck", "Remove from library"), "Delete"}}}};
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
                    "Fora.Deck", "Wayland shortcuts request permission through the GlobalShortcuts portal when "
                                 "supported. If refused or unavailable, bind foradeck --overlay in system "
                                 "keyboard settings: KDE: Keyboard → Shortcuts; GNOME: Keyboard → Custom "
                                 "Shortcuts; Xfce: Keyboard → Application Shortcuts. Use foradeck for the library."),
                "muted"));
#endif
        layout->addStretch();
        pages->addWidget(contents);
    }
    auto *service = static_cast<DesktopService *>(context.service);
    for (const auto scenario : {false, true}) {
        tabs->addTab(scenario ? QCoreApplication::translate("Fora.Deck", "Scenarios")
                              : QCoreApplication::translate("Fora.Deck", "Applications"));
        auto *contents = new QWidget;
        auto *layout = new QVBoxLayout(contents);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        layout->addWidget(
            label(QCoreApplication::translate(
                      "Fora.Deck",
                      "Assign a global shortcut in the application or scenario editor. An empty shortcut disables it."),
                  "muted"));
        auto *targets = new QTreeWidget;
        targets->setRootIsDecorated(false);
        targets->setHeaderLabels({QCoreApplication::translate("Fora.Deck", "Name"),
                                  QCoreApplication::translate("Fora.Deck", "Keyboard shortcuts")});
        targets->header()->setStretchLastSection(false);
        targets->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        targets->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        targets->setMinimumHeight(appearance::scaled(*context.settings, 160));
        targets->setUniformRowHeights(true);
        layout->addWidget(targets, 1);
        auto *edit = button(QCoreApplication::translate("Fora.Deck", "Edit"), "quiet");
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
                    targets, {name, sequence.isEmpty() ? QCoreApplication::translate("Fora.Deck", "Not assigned")
                                                       : sequence.toString(QKeySequence::NativeText)});
                item->setData(0, Qt::UserRole, id);
                item->setToolTip(0, QStringLiteral("foradeck ") +
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
            label(QCoreApplication::translate("Fora.Deck", "On Wayland, configure a desktop shortcut for foradeck "
                                                           "--launch-id <id> or foradeck --scenario-id <id>."),
                  "muted"));
#endif
        pages->addWidget(contents);
    }
    QObject::connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    QObject::connect(pages, &QStackedWidget::currentChanged, tabs, &QTabBar::setCurrentIndex);
    QObject::connect(pages, &QStackedWidget::currentChanged, transition, [transition] { transition->start(); });
    widget->layout()->addWidget(label(
        QCoreApplication::translate("Fora.Deck",
                                    "An empty field disables the shortcut. Application actions use the "
                                    "selected library entry. Text editing keys keep their usual behavior "
                                    "in text fields. ↑↓ selects a row. Escape clears the search and is reserved."),
        "muted"));
    return widget;
}

QWidget *library_settings(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Application sources"));
    widget->layout()->addWidget(action_button(context.actions.value("library_health")));
#ifdef Q_OS_WIN
    checkbox(widget, *context.settings, "sources/registry",
             QCoreApplication::translate("Fora.Deck", "Registered Windows applications"), true);
    checkbox(widget, *context.settings, "sources/start_menu",
             QCoreApplication::translate("Fora.Deck", "Start menu shortcuts"), true);
#else
    checkbox(widget, *context.settings, "sources/registry",
             QCoreApplication::translate("Fora.Deck", "Linux system applications"), true);
    checkbox(widget, *context.settings, "sources/start_menu",
             QCoreApplication::translate("Fora.Deck", "Linux user applications"), true);
#endif
    widget->layout()->addWidget(label(
        QCoreApplication::translate(
            "Fora.Deck", "Applications are checked in the background at every startup. New applications are offered "
                         "for selection. Applications removed from the library are not offered again. "
                         "Changed sources will be used after saving settings."),
        "muted"));
    checkbox(widget, *context.settings, "sources/confirm_removal",
             QCoreApplication::translate("Fora.Deck", "Ask before removing uninstalled applications from the library"),
             true);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck",
                                          "When disabled, confirmed missing applications are removed automatically. "
                                          "Unavailable drives and uncertain targets are kept."),
              "muted"));
    checkbox(widget, *context.settings, "sources/remove_unavailable_without_icons",
             QCoreApplication::translate("Fora.Deck", "Automatically remove unavailable applications without icons"),
             true);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck", "Runs at startup and during application scans."), "muted"));
    widget->layout()->addWidget(action_button(context.actions.value("discover")));
    auto *clear_exclusions = button(QCoreApplication::translate("Fora.Deck", "Reset discovery exclusions"));
    widget->layout()->addWidget(clear_exclusions);
    QObject::connect(clear_exclusions, &QPushButton::clicked, service, &DesktopService::clear_discovery_exclusions);
    widget->layout()->addWidget(context.registry->create("discovery_progress", context, widget));
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck", "Library import and export"), "subheading"));
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck",
                  "JSON saves applications, parameters, favorites, and tags. Review the entries and conflict policy "
                  "before importing. Matching configurations are skipped by default. Applications are never launched "
                  "automatically."),
              "muted"));
    auto *import = button(QCoreApplication::translate("Fora.Deck", "Import…"), nullptr, "import");
    auto *export_button = button(QCoreApplication::translate("Fora.Deck", "Export…"), nullptr, "export");
    widget->layout()->addWidget(import);
    widget->layout()->addWidget(export_button);
    QObject::connect(import, &QPushButton::clicked, widget, [=] {
        const auto path =
            QFileDialog::getOpenFileName(widget, QCoreApplication::translate("Fora.Deck", "Import library"), {},
                                         QCoreApplication::translate("Fora.Deck", "Fora.Deck catalog (*.json)"));
        if (!path.isEmpty())
            service->import_catalog(path);
    });
    QObject::connect(export_button, &QPushButton::clicked, widget, [=] {
        const auto path = QFileDialog::getSaveFileName(
            widget, QCoreApplication::translate("Fora.Deck", "Export library"), "foradeck.json",
            QCoreApplication::translate("Fora.Deck", "Fora.Deck catalog (*.json)"));
        if (!path.isEmpty())
            service->export_catalog(path);
    });
    auto *clear = button(QCoreApplication::translate("Fora.Deck", "Clear usage history"));
    widget->layout()->addWidget(clear);
    QObject::connect(clear, &QPushButton::clicked, widget, [=] {
        if (confirm(widget, QCoreApplication::translate("Fora.Deck", "Usage history"),
                    QCoreApplication::translate("Fora.Deck",
                                                "Clear application and file usage counts and recent launch "
                                                "times? Applications and tags will be kept."),
                    QCoreApplication::translate("Fora.Deck", "Clear")))
            service->clear_history();
    });
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *storage_page(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Local data"));
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck",
                  "Your library, tags, scenarios and settings are stored on this computer. Fora.Deck does not upload "
                  "your catalog. Update checks contact GitHub; your saved access token is kept in the system "
                  "credential store."),
              "muted"));
    const auto add_path = [=](const QString &title, const QString &path) {
        widget->layout()->addWidget(label(title, "subheading"));
        auto *value = label(QDir::toNativeSeparators(path), "muted");
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        widget->layout()->addWidget(value);
    };
    add_path(QCoreApplication::translate("Fora.Deck", "Settings file"), context.settings->fileName());
    auto *database = label("", "muted");
    database->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    widget->layout()->addWidget(label(QCoreApplication::translate("Fora.Deck", "Catalog database"), "subheading"));
    widget->layout()->addWidget(database);
    const auto update_path = [=] { database->setText(QDir::toNativeSeparators(service->library_path())); };
    QObject::connect(service, &DesktopService::catalog_changed, widget, update_path);
    update_path();
    widget->layout()->addWidget(label(application_paths::portable_mode()
                                          ? QCoreApplication::translate("Fora.Deck", "Current mode: portable")
                                          : QCoreApplication::translate("Fora.Deck", "Current mode: installed"),
                                      "subheading"));
    widget->layout()->addWidget(label(
        QCoreApplication::translate(
            "Fora.Deck",
            "Installed mode uses your operating system's user data folders. A portable.flag next to the executable "
            "selects the data folder beside it. An AppImage uses <AppImage>.data beside the image. Moving the "
            "executable "
            "alone does not transfer installed-mode data; use a complete profile backup to move your library."),
        "muted"));
    auto *folder = button(QCoreApplication::translate("Fora.Deck", "Open profile folder"));
    widget->layout()->addWidget(folder);
    QObject::connect(folder, &QPushButton::clicked, service, &DesktopService::open_profile_folder);
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *file_settings(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "File search"));
    auto *mode = new QComboBox;
    mode->setObjectName("file_search_mode");
    mode->setProperty("setting_key", "files/mode");
    mode->addItem(QCoreApplication::translate("Fora.Deck", "Off"), "off");
    mode->addItem(QCoreApplication::translate("Fora.Deck", "Basic"), "basic");
    mode->addItem(QCoreApplication::translate("Fora.Deck", "Full (not recommended)"), "full");
    mode->setCurrentIndex(qMax(0, mode->findData(context.settings->value("files/mode", "off").toString())));
    field(static_cast<QVBoxLayout *>(widget->layout()), QCoreApplication::translate("Fora.Deck", "Indexing mode"),
          mode);
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck",
                  "Off by default. Basic: Desktop, Documents and Downloads, including all subfolders; "
                  "other local drives are checked only at the root and one folder level below it (max-depth=1). "
                  "Full: all accessible local drives recursively. Not recommended: indexing can take a long time."),
              "muted"));
    auto *limit_preset = new QComboBox;
    limit_preset->setObjectName("file_result_limit_preset");
    for (const int limit : {50, 100, FileManager::default_result_limit, 500, 1000})
        limit_preset->addItem(QString::number(limit), limit);
    limit_preset->addItem(QCoreApplication::translate("Fora.Deck", "Custom"), 0);
    auto *result_limit = new QSpinBox;
    result_limit->setObjectName("file_result_limit");
    result_limit->setProperty("setting_key", "files/result_limit");
    result_limit->setRange(1, FileManager::max_result_limit);
    result_limit->setValue(context.settings->value("files/result_limit", FileManager::default_result_limit).toInt());
    const auto preset = limit_preset->findData(result_limit->value());
    limit_preset->setCurrentIndex(preset >= 0 ? preset : limit_preset->count() - 1);
    auto *limit_control = new QWidget;
    limit_control->setFocusProxy(limit_preset);
    auto *limit_layout = new QHBoxLayout(limit_control);
    limit_layout->setContentsMargins(0, 0, 0, 0);
    limit_layout->addWidget(limit_preset);
    limit_layout->addWidget(result_limit);
    result_limit->setAccessibleName(QCoreApplication::translate("Fora.Deck", "Custom file result limit"));
    result_limit->setVisible(preset < 0);
    QObject::connect(limit_preset, &QComboBox::currentIndexChanged, result_limit, [=] {
        const auto value = limit_preset->currentData().toInt();
        result_limit->setVisible(value == 0);
        if (value)
            result_limit->setValue(value);
        else
            result_limit->setFocus();
    });
    const auto limit_label = QCoreApplication::translate("Fora.Deck", "Maximum results in the Files tab");
    limit_preset->setAccessibleName(limit_label);
    field(static_cast<QVBoxLayout *>(widget->layout()), limit_label, limit_control);
    const auto path_list = [=](const QString &title, const QString &key, const bool allow_files) {
        widget->layout()->addWidget(label(title, "subheading"));
        auto *paths = new QListWidget;
        paths->setObjectName(allow_files ? "file_search_exclusions" : "file_search_roots");
        paths->setProperty("setting_key", key);
        paths->setAccessibleName(title);
        paths->setMaximumHeight(120);
        paths->addItems(context.settings->value(key).toStringList());
        widget->layout()->addWidget(paths);
        auto *tools = new QHBoxLayout;
        auto *add = button(QCoreApplication::translate("Fora.Deck", "Add folder"));
        auto *remove = button(QCoreApplication::translate("Fora.Deck", "Remove selected"));
        tools->addWidget(add);
        const auto append = [=](const QString &path) {
            if (!path.isEmpty() && paths->findItems(path, Qt::MatchExactly).isEmpty())
                paths->addItem(path);
        };
        QObject::connect(add, &QPushButton::clicked, widget, [=] {
            append(QFileDialog::getExistingDirectory(widget, QCoreApplication::translate("Fora.Deck", "Add folder")));
        });
        if (allow_files) {
            auto *file = button(QCoreApplication::translate("Fora.Deck", "Add file"));
            tools->addWidget(file);
            QObject::connect(file, &QPushButton::clicked, widget, [=] {
                const auto selected =
                    QFileDialog::getOpenFileNames(widget, QCoreApplication::translate("Fora.Deck", "Add file"));
                for (const auto &path : selected)
                    append(path);
            });
        }
        tools->addWidget(remove);
        tools->addStretch();
        QObject::connect(remove, &QPushButton::clicked, widget, [=] { delete paths->takeItem(paths->currentRow()); });
        static_cast<QVBoxLayout *>(widget->layout())->addLayout(tools);
    };
    path_list(QCoreApplication::translate("Fora.Deck", "Additional recursive folders"), "files/roots", false);
    path_list(QCoreApplication::translate("Fora.Deck", "Excluded folders and files"), "files/exclusions", true);
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck",
                  "Additional folders expand the selected scope. Exclusions are applied afterwards and always win, "
                  "including inside additional folders. Excluding a folder excludes all its subfolders. "
                  "These rules take effect after Save."),
              "muted"));
    auto *status = label(service->files->status(), "muted");
    status->setObjectName("file_index_status");
    widget->layout()->addWidget(status);
    QObject::connect(service->files, &FileManager::status_changed, widget,
                     [=] { status->setText(service->files->status()); });
    auto *diagnostics = new QPlainTextEdit(service->files->diagnostics_text());
    diagnostics->setObjectName("file_index_diagnostics");
    diagnostics->setReadOnly(true);
    diagnostics->setAccessibleName(QCoreApplication::translate("Fora.Deck", "File index diagnostics"));
    diagnostics->setMinimumHeight(appearance::scaled(*context.settings, 180));
    widget->layout()->addWidget(diagnostics);
    QObject::connect(service->files, &FileManager::status_changed, diagnostics,
                     [=] { diagnostics->setPlainText(service->files->diagnostics_text()); });
    widget->layout()->addWidget(label(
        QCoreApplication::translate("Fora.Deck", "Unavailable sources are checked about every 30 seconds. Returning "
                                                 "sources refresh separately from the regular index refresh."),
        "muted"));
    auto *scan = button(QCoreApplication::translate("Fora.Deck", "Refresh file index"));
    scan->setEnabled(service->files->is_enabled());
    widget->layout()->addWidget(scan);
    QObject::connect(service->files, &FileManager::configuration_changed, scan,
                     [=] { scan->setEnabled(service->files->is_enabled()); });
    QObject::connect(scan, &QPushButton::clicked, service->files, &FileManager::rescan);
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck",
                  "The index is saved locally and refreshes no sooner than every 6 hours, or on manual request. "
                  "Search remains available during indexing. New files appear on your next search; the current results "
                  "stay in place. "
                  "Supported working-file formats are indexed; unavailable locations and symbolic links are skipped."),
              "muted"));
    widget->layout()->addWidget(label(
        QCoreApplication::translate("Fora.Deck",
                                    "Filename masks: * matches any number of characters; ? matches one character. "
                                    "Examples: *.pdf, report-??.txt. Other characters are literal."),
        "muted"));
    return widget;
}

QWidget *plugins(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Interface modules"));
    widget->layout()->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck",
                  "Native plugins can change interface areas and appearance. Enable only trusted modules; they run "
                  "inside Fora.Deck. Changes take effect after restart."),
              "muted"));
    auto available = context.parameters.value("available_plugins").toStringList();
    const auto enabled = context.settings->value("plugins/enabled").toStringList();
    if (service->safe_mode()) {
        widget->layout()->addWidget(
            label(QCoreApplication::translate(
                      "Fora.Deck",
                      "Safe mode skips native modules. Saved enabled modules are listed below so you can disable "
                      "a faulty module before restarting."),
                  "muted"));
        for (const auto &id : enabled)
            if (!available.contains(id))
                available.append(id);
    }
    const auto errors = context.parameters.value("plugin_errors").toMap();
    const auto metadata = context.parameters.value("plugin_metadata").toMap();
    for (const auto &id : available) {
        const auto details = metadata.value(id).toMap();
        const auto value = [&](const QString &key, const QString &alternate) {
            auto text = details.value(key);
            if (!text.isValid())
                text = details.value(alternate);
            return text.metaType().id() == QMetaType::QString ? text.toString().trimmed().left(256) : QString{};
        };
        const auto name = value("name", "Name");
        const auto version = value("version", "Version");
        const auto author = value("author", "Author");
        auto *control = new QCheckBox(name.isEmpty() ? id : name);
        control->setProperty("plugin_id", id);
        control->setChecked(enabled.contains(id));
        widget->layout()->addWidget(control);
        QStringList information;
        if (!name.isEmpty())
            information.append(id);
        if (!version.isEmpty())
            information.append(QCoreApplication::translate("Fora.Deck", "Version %1").arg(version));
        if (!author.isEmpty())
            information.append(QCoreApplication::translate("Fora.Deck", "Author: %1").arg(author));
        if (!information.isEmpty())
            widget->layout()->addWidget(label(information.join(" · "), "muted"));
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
            label(QCoreApplication::translate(
                      "Fora.Deck", "No modules installed. Folder: plugins next to the Fora.Deck executable."),
                  "muted"));
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *help_page(const ui_context &context, QWidget *) {
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Quick help"));
    widget->layout()->addWidget(action_button(context.actions.value("tutorial")));
    const QList<QPair<QString, QString>> topics{
        {QCoreApplication::translate("Fora.Deck", "Search"),
         QCoreApplication::translate("Fora.Deck",
                                     "Type an application name, alias or filename. Use #work to require a tag and "
                                     "-#games to exclude one; combine them with text. "
                                     "Clear filters to search the whole library. Quick search also accepts file paths "
                                     "and calculations such as (2 + 3) * 4; Enter copies the result.")},
        {QCoreApplication::translate("Fora.Deck", "Tags"),
         QCoreApplication::translate("Fora.Deck",
                                     "Create tags in the sidebar or application editor. A tag has up to 64 "
                                     "characters without spaces, commas or #. "
                                     "Select several applications with Ctrl or Shift to edit their tags "
                                     "together. Removing a tag keeps the applications.")},
        {QCoreApplication::translate("Fora.Deck", "Scenarios"),
         QCoreApplication::translate("Fora.Deck", "Create a scenario from the Scenarios section. Add applications in "
                                                  "launch order, set each delay and choose a launch profile. "
                                                  "Disabled steps are skipped. Fix the listed problems before running. "
                                                  "Success means the launch request was accepted, "
                                                  "not that the application finished loading. Stop skips remaining "
                                                  "steps and keeps already opened applications running.")},
        {QCoreApplication::translate("Fora.Deck", "Keyboard shortcuts"),
         QCoreApplication::translate(
             "Fora.Deck",
             "Open Settings → Keyboard shortcuts to see or change the current bindings. Clear a binding to disable it. "
             "Assign application and scenario shortcuts in their editors. A shortcut already used by Fora.Deck or "
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
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "About"));
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
    title->addWidget(label(QGuiApplication::applicationDisplayName(), "heading"));
    auto *version =
        label(QCoreApplication::translate("Fora.Deck", "Version ") + QCoreApplication::applicationVersion(), "muted");
    version->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    title->addWidget(version);
    title->addWidget(label(QCoreApplication::translate("Fora.Deck", "By ForaSource"), "muted"));
    identity->addLayout(title, 1);
    card_layout->addLayout(identity);
    card_layout->addWidget(
        label(QCoreApplication::translate("Fora.Deck", "Application library for Windows and Linux"), "subheading"));
    card_layout->addWidget(
        label(QCoreApplication::translate("Fora.Deck",
                                          "Applications, tags, and scenarios in one place. The catalog is stored "
                                          "locally; no internet connection is required."),
              "muted"));
    auto *github = button(QCoreApplication::translate("Fora.Deck", "Open GitHub ↗"));
    auto *issues = button(QCoreApplication::translate("Fora.Deck", "Report an issue ↗"));
    auto *documentation = button(QCoreApplication::translate("Fora.Deck", "Open documentation ↗"));
    auto *copy = button(QCoreApplication::translate("Fora.Deck", "Copy version and environment"));
    card_layout->addWidget(github);
    card_layout->addWidget(issues);
    card_layout->addWidget(documentation);
    card_layout->addWidget(copy);
    QObject::connect(github, &QPushButton::clicked, widget,
                     [] { desktop_platform::open_url(QUrl("https://github.com/ForaSource/Fora.Deck")); });
    QObject::connect(issues, &QPushButton::clicked, widget,
                     [] { desktop_platform::open_url(QUrl("https://github.com/ForaSource/Fora.Deck/issues")); });
    QObject::connect(documentation, &QPushButton::clicked, widget, [] {
        desktop_platform::open_url(QUrl("https://github.com/ForaSource/Fora.Deck/blob/main/README.md"));
    });
    QObject::connect(copy, &QPushButton::clicked, widget, [=] {
        const auto text =
            QCoreApplication::translate("Fora.Deck",
                                        "Fora.Deck %1\nOS: %2\nArchitecture: %3\nQt: %4\nSession: %5\nMode: %6")
                .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(),
                     QSysInfo::currentCpuArchitecture(), QString::fromLatin1(qVersion()),
                     QGuiApplication::platformName(),
                     application_paths::portable_mode() ? QStringLiteral("portable") : QStringLiteral("installed"));
        QGuiApplication::clipboard()->setText(text);
        static_cast<DesktopService *>(context.service)
            ->notify(QCoreApplication::translate("Fora.Deck", "Version and environment copied."));
    });
    widget->layout()->addWidget(card);
    widget->layout()->addWidget(
        label(QCoreApplication::translate("Fora.Deck", "Technologies and licenses"), "subheading"));
    widget->layout()->addWidget(
        label(QString(QCoreApplication::translate(
                          "Fora.Deck", "Qt %1 · C++23 · SQLite · QtKeychain · Inter\nFora.Deck — "
                                       "MIT; Inter — OFL 1.1\nThird-party licenses are included with the application."))
                  .arg(QString::fromLatin1(qVersion())),
              "muted"));
    static_cast<QVBoxLayout *>(widget->layout())->addStretch();
    return widget;
}

QWidget *update_page(const ui_context &context, QWidget *) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = page(QCoreApplication::translate("Fora.Deck", "Update"));
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
    channel->addItem(QCoreApplication::translate("Fora.Deck", "Stable"), "stable");
    channel->addItem(QCoreApplication::translate("Fora.Deck", "Preview"), "preview");
    channel->setCurrentIndex(channel->findData(updates->channel()));
    field(card_layout, QCoreApplication::translate("Fora.Deck", "Update channel"), channel);
    card_layout->addWidget(
        label(QCoreApplication::translate(
                  "Fora.Deck", "Stable includes published stable releases. Preview also includes pre-releases. "
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
    token->setPlaceholderText(QCoreApplication::translate("Fora.Deck", "Paste a GitHub Personal Access Token"));
    field(card_layout, QCoreApplication::translate("Fora.Deck", "Personal GitHub token (optional)"), token);
    card_layout->addWidget(label(
        QCoreApplication::translate("Fora.Deck", "For public releases, use a token without additional permissions. "
                                                 "It is sent only to the GitHub API."),
        "muted"));
    auto *token_buttons = new QHBoxLayout;
    auto *save_token = button(QCoreApplication::translate("Fora.Deck", "Save token"));
    auto *remove_token = button(QCoreApplication::translate("Fora.Deck", "Delete token"));
    // Pressing Enter in the token field must not save the token instead of the regular settings.
    save_token->setAutoDefault(false);
    remove_token->setAutoDefault(false);
    token_buttons->addWidget(save_token);
    token_buttons->addWidget(remove_token);
    card_layout->addLayout(token_buttons);
    auto *token_status = label(
        tokens->has_token()
            ? QCoreApplication::translate("Fora.Deck", "A personal token is saved in the system secret store.")
            : QCoreApplication::translate("Fora.Deck", "No token saved. Update checks use unauthenticated requests."),
        "muted");
    card_layout->addWidget(token_status);
    card_layout->addWidget(label(
        QCoreApplication::translate("Fora.Deck", "Save token and Delete token take effect immediately. "
                                                 "The token is not stored in the settings file or library export."),
        "muted"));
    auto *check_update = button(QCoreApplication::translate("Fora.Deck", "Check for updates"));
    auto *update_status =
        label(QCoreApplication::translate(
                  "Fora.Deck", "Checks GitHub Releases at startup and on request for the selected channel. "
                               "Results are cached for 60 seconds without a token or 5 seconds with a token."),
              "muted");
    auto *update_progress = new QProgressBar;
    update_progress->setAccessibleName(QCoreApplication::translate("Fora.Deck", "Update download progress"));
    update_progress->hide();
    auto *cancel_update = button(QCoreApplication::translate("Fora.Deck", "Cancel"));
    cancel_update->hide();
    auto *release_button =
        button(QCoreApplication::translate("Fora.Deck", "Open release on GitHub ↗"), nullptr, "external");
    release_button->hide();
    auto *install_update = button(QCoreApplication::translate("Fora.Deck", "Update now"), "primary", "download");
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
        update_status->setText(QCoreApplication::translate("Fora.Deck", "Channel changed. Check for updates."));
    });
    auto open_release = [=] {
        if (!desktop_platform::open_url(updates->available_release_url()))
            update_status->setText(
                QCoreApplication::translate("Fora.Deck", "Could not open the release in a browser."));
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
        token_status->setText(QCoreApplication::translate("Fora.Deck", "Saving token…"));
        tokens->save(token->text());
    });
    QObject::connect(remove_token, &QPushButton::clicked, widget, [=] {
        token_status->setText(QCoreApplication::translate("Fora.Deck", "Deleting token…"));
        tokens->remove();
    });
    QObject::connect(tokens, &GithubTokenStore::changed, widget, [=](const bool stored) {
        token->clear();
        token_status->setText(
            stored ? QCoreApplication::translate("Fora.Deck", "A personal token is saved in the system secret store.")
                   : QCoreApplication::translate("Fora.Deck",
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
        update_status->setText(QCoreApplication::translate("Fora.Deck", "Checking for updates…"));
        update_progress->setRange(0, 0);
        update_progress->show();
        tokens->read();
    });
    QObject::connect(cancel_update, &QPushButton::clicked, widget, [=] {
        updates->cancel();
        update_status->setText(QCoreApplication::translate("Fora.Deck", "Update cancelled."));
    });
    QObject::connect(updates, &UpdateChecker::failed, widget, [=](const QString &message) {
        update_progress->hide();
        update_status->setText(message);
    });
    QObject::connect(updates, &UpdateChecker::operation_changed, widget, [=](const operation_result &result) {
        if (result.state == operation_state::cancelled) {
            update_progress->hide();
            update_status->setText(QCoreApplication::translate("Fora.Deck", "Update cancelled."));
        }
    });
    QObject::connect(updates, &UpdateChecker::up_to_date, widget, [=](const bool releases_exist) {
        update_status->setText(
            releases_exist
                ? QCoreApplication::translate("Fora.Deck", "You are using the latest available version.")
                : QCoreApplication::translate("Fora.Deck", "No releases with a supported version were found."));
    });
    QObject::connect(
        updates, &UpdateChecker::download_progress, widget, [=](const qint64 received, const qint64 total) {
            update_progress->setRange(0, 100);
            update_progress->setValue(static_cast<int>(received * 100 / total));
            update_progress->show();
            update_status->setText(QCoreApplication::translate("Fora.Deck", "Downloading update… %1 / %2 MiB")
                                       .arg(received / (1024.0 * 1024.0), 0, 'f', 1)
                                       .arg(total / (1024.0 * 1024.0), 0, 'f', 1));
        });
    auto show_available_update = [=] {
        release_button->show();
        install_update->setText(
            QCoreApplication::translate("Fora.Deck", updates->can_install() ? "Update now" : "Open release"));
        const auto icon_name = updates->can_install() ? "download" : "external";
        install_update->setProperty("appearance_icon", icon_name);
        install_update->setIcon(appearance::icon(icon_name, QColor("#181A1D")));
        install_update->show();
        update_status->setText(QCoreApplication::translate("Fora.Deck", "New version available: %1 (%2 channel)")
                                   .arg(updates->available_version(), channel->currentText()));
    };
    QObject::connect(updates, &UpdateChecker::update_available, widget, show_available_update);
    if (!updates->available_version().isEmpty())
        show_available_update();
    if (updates->is_busy()) {
        update_status->setText(QCoreApplication::translate("Fora.Deck", "Checking for updates…"));
        update_progress->setRange(0, 0);
        update_progress->show();
        cancel_update->show();
        release_button->setEnabled(false);
    }
    QObject::connect(install_update, &QPushButton::clicked, widget, [=] {
        if (updates->is_busy() || tokens->is_busy() || updates->available_version().isEmpty())
            return;
        if (updates->can_install()) {
            if (confirm(widget, QCoreApplication::translate("Fora.Deck", "Fora.Deck update"),
                        QCoreApplication::translate(
                            "Fora.Deck", "A new version is available: %1 (current: %2). Update now?\n\n"
                                         "Fora.Deck will download and verify the installer, then close. "
                                         "Complete the update in the installation wizard. Your library and settings "
                                         "will be kept. Save any unsaved edits before continuing.")
                            .arg(updates->available_version(), QCoreApplication::applicationVersion()),
                        QCoreApplication::translate("Fora.Deck", "Update now"))) {
                *request_started = true;
                updates->download();
            }
        } else if (confirm(widget, QCoreApplication::translate("Fora.Deck", "Fora.Deck update"),
                           QCoreApplication::translate(
                               "Fora.Deck",
                               "A new version is available: %1 (current: %2). Open the release on GitHub?\n\n"
                               "Automatic installation is unavailable for this copy or release. "
                               "Follow the release instructions and keep your existing profile.")
                               .arg(updates->available_version(), QCoreApplication::applicationVersion()),
                           QCoreApplication::translate("Fora.Deck", "Open release"))) {
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
    dialog->setWindowTitle(QCoreApplication::translate("Fora.Deck", "Fora.Deck settings"));
    prepare_dialog(dialog, context, {960, 700}, {640, 420});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);
    root->addWidget(label(QCoreApplication::translate("Fora.Deck", "Settings"), "heading"));
    root->addWidget(
        label(QCoreApplication::translate("Fora.Deck", "Fora.Deck appearance, behavior, and features"), "muted"));
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
        {QCoreApplication::translate("Fora.Deck", "General"), "settings_general"},
        {QCoreApplication::translate("Fora.Deck", "Appearance settings"), "settings_appearance"},
        {QCoreApplication::translate("Fora.Deck", "Keyboard shortcuts"), "settings_hotkeys"},
        {QCoreApplication::translate("Fora.Deck", "Library"), "settings_library"},
        {QCoreApplication::translate("Fora.Deck", "Profile"), "settings_profile"},
        {QCoreApplication::translate("Fora.Deck", "Local data"), "settings_storage"},
        {QCoreApplication::translate("Fora.Deck", "File search"), "settings_files"},
        {QCoreApplication::translate("Fora.Deck", "Plugins"), "settings_plugins"},
        {QCoreApplication::translate("Fora.Deck", "Update"), "settings_update"},
        {QCoreApplication::translate("Fora.Deck", "Quick help"), "settings_help"},
        {QCoreApplication::translate("Fora.Deck", "About"), "settings_about"}};
    for (const auto &section : sections) {
        const auto index = pages->count();
        auto *item = new SelectionButton(section.first);
        const QHash<QString, QString> icons{
            {"settings_general", "settings"}, {"settings_appearance", "palette"}, {"settings_hotkeys", "keyboard"},
            {"settings_library", "library"},  {"settings_profile", "profile"},    {"settings_storage", "database"},
            {"settings_files", "file"},       {"settings_plugins", "plugin"},     {"settings_update", "refresh"},
            {"settings_help", "help"},        {"settings_about", "info"}};
        item->setIcon(appearance::icon(icons.value(section.second), item->palette().color(QPalette::ButtonText)));
        item->setProperty("appearance_icon", icons.value(section.second));
        const auto icon_size = appearance::scaled(*context.settings, 20);
        item->setIconSize({icon_size, icon_size});
        item->setObjectName(section.second + "_category");
        categories->addButton(item, index);
        navigation_layout->addWidget(item);
        auto *holder = new QWidget;
        auto *page_layout = new QHBoxLayout(holder);
        page_layout->setContentsMargins(0, 0, 0, 0);
        auto *contents = context.registry->create(section.second, context, holder);
        contents->setMaximumWidth(appearance::scaled(*context.settings, 760));
        page_layout->addWidget(contents, 1);
        page_layout->addStretch();
        pages->addWidget(scroll(holder));
    }
    navigation_layout->addStretch();
    auto *navigation_scroll = scroll(navigation);
    navigation_scroll->setObjectName("settings_navigation");
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
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("Fora.Deck", "Save"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("Fora.Deck", "Cancel"));
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
                "Fora.Deck",
                "Settings cannot be saved while a profile operation is running or a restart is required."));
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
            if (auto *slider = qobject_cast<QSlider *>(control))
                values.insert(key, slider->value());
            if (auto *spin = qobject_cast<QSpinBox *>(control)) {
                spin->interpretText();
                values.insert(key, spin->value());
            }
            if (auto *list = qobject_cast<QListWidget *>(control)) {
                QStringList paths;
                for (int i = 0; i < list->count(); ++i) {
                    const QFileInfo entry(list->item(i)->text());
                    if (!entry.isAbsolute() || (key == "files/roots" && entry.exists() && !entry.isDir())) {
                        show_error(QCoreApplication::translate("Fora.Deck", "Choose an absolute file or folder path."));
                        return;
                    }
                    const auto canonical = entry.canonicalFilePath();
                    paths.append(canonical.isEmpty() ? QDir::cleanPath(entry.absoluteFilePath()) : canonical);
                }
                paths.removeDuplicates();
                values.insert(key, paths);
            }
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
            show_error(QCoreApplication::translate("Fora.Deck", "Could not save the settings file."));
            return;
        }
        if (previous.value("files/roots") != values.value("files/roots") ||
            previous.value("files/exclusions") != values.value("files/exclusions") ||
            previous.value("files/mode") != values.value("files/mode"))
            service->files->configure(FileManager::mode_from_setting(values.value("files/mode").toString()),
                                      values.value("files/roots").toStringList(),
                                      values.value("files/exclusions").toStringList());
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
    registry.add_default("settings_files", file_settings);
    registry.add_default("settings_plugins", plugins);
    registry.add_default("settings_update", update_page);
    registry.add_default("settings_about", about);
    registry.add_default("settings_help", help_page);
}
