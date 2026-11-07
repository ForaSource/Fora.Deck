#ifndef XLIBRARY_PLATFORM_INTEGRATION_H
#define XLIBRARY_PLATFORM_INTEGRATION_H

#include <atomic>
#include <functional>
#include <memory>
#include <QAbstractNativeEventFilter>
#include <QImage>
#include <QKeySequence>
#include <QObject>
#include <QSet>

#include "Application.h"
#include "CatalogTypes.h"

class QWidget;
class QUrl;
#ifdef Q_OS_LINUX
class LinuxHotkeyState;
#endif

namespace desktop_platform {
QString default_global_shortcut();
QString default_overlay_shortcut();
void activate_overlay(QWidget *window);
using launch_mode = Application::launch_mode;
void launch_application(const app_record &record, launch_mode mode);

void open_folder(const QString &path);
bool open_url(const QUrl &url);
bool installed_for_updates();
bool start_update_installer(const QString &path, QString &error);
QImage application_icon(const QString &path);

bool set_autostart(bool enabled, QString &error);
bool animations_enabled();
void apply_backdrop(QWidget *window, bool dark, bool glass);
}

class GlobalHotkey : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
private:
    int current_id = 0;
    inline static int next_id = 1;
    QKeySequence sequence;
#ifdef Q_OS_LINUX
    std::unique_ptr<LinuxHotkeyState> linux_state;
#endif

public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;
    bool set_sequence(const QKeySequence &value, QString &error);
    bool nativeEventFilter(const QByteArray &event_type, void *message, qintptr *result) override;

signals:
    void activated();
    void registration_failed(const QString &message);
};

#endif
