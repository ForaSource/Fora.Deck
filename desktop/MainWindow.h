#ifndef XLIBRARY_MAIN_WINDOW_H
#define XLIBRARY_MAIN_WINDOW_H

#include <memory>
#include <QMainWindow>
#include <QPluginLoader>
#include <QSystemTrayIcon>
#include <vector>

#include "DesktopService.h"
#include "PlatformIntegration.h"
#include "SearchOverlay.h"
#include "UiRegistry.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
private:
    DesktopService &service;
    UiRegistry registry;
    ui_context context;
    GlobalHotkey hotkey;
    GlobalHotkey overlay_hotkey;
    std::unique_ptr<SearchOverlay> overlay;
    QSystemTrayIcon tray;
    std::vector<std::unique_ptr<QPluginLoader>> plugins;
    bool quitting = false;
    bool welcome_offered = false;
    void show_welcome();
    void show_tutorial();
    void create_actions();
    void refresh_actions();
    void apply_settings();
    void load_plugins();
    void update_layout();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

public:
    explicit MainWindow(DesktopService &service);
    ~MainWindow() override;
    void show_library();
    void show_overlay();
    void start();
};

#endif
