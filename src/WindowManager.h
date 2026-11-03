#ifndef XLIBRARY_WINDOWMANAGER_H
#define XLIBRARY_WINDOWMANAGER_H

#include <QString>

#include "CatalogTypes.h"

// Theme policy and native activation; drawing remains in the desktop integration.
class WindowManager {
private:
    bool dark_theme = true;
    double threshold = 150.0;
    double brightness = -1;

    double get_screen_brightness() const;

public:
    void update_theme();
    void update_theme(const QString &preference, bool system_light);
    static bool activate_window(const app_record &record);
    int get_theme() const { return this->dark_theme; }
};

#endif //XLIBRARY_WINDOWMANAGER_H
