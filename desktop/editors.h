#ifndef XLIBRARY_EDITORS_H
#define XLIBRARY_EDITORS_H

#include "UiRegistry.h"

class DesktopService;
void register_editors(UiRegistry &registry);
void edit_application(DesktopService &service, qint64 id, QWidget *parent, const QString &focus_field = {});

#endif
