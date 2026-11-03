#ifndef XLIBRARY_PROFILE_VIEW_H
#define XLIBRARY_PROFILE_VIEW_H

#include "UiRegistry.h"

class DesktopService;
struct app_record;

QWidget *profile_view(const ui_context &context, QWidget *parent);
void select_profile_restore(const ui_context &context, QWidget *parent);
void select_application_replacement(DesktopService *service, const app_record &record, QWidget *parent);

#endif
