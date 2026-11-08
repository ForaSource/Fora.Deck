#ifndef XLIBRARY_SEARCH_TOOLS_H
#define XLIBRARY_SEARCH_TOOLS_H

class DesktopService;
class QLineEdit;
class QWidget;

QWidget *search_tools(DesktopService &service, QLineEdit &input, QWidget *parent);

class QAbstractButton;
QAbstractButton *file_search_switch(bool enabled, QWidget *parent);

#endif
