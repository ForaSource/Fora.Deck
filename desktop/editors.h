#ifndef XLIBRARY_EDITORS_H
#define XLIBRARY_EDITORS_H

#include "UiRegistry.h"

class DesktopService;
void register_editors(UiRegistry &registry);
void edit_application(DesktopService &service, qint64 id, QWidget *parent, const QString &focus_field = {});
qint64 add_application_from_file(DesktopService &service, const QString &path, QWidget *parent);
void confirm_application_termination(DesktopService &service, qint64 id, QWidget *parent);

#endif
