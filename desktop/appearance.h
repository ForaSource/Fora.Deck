#ifndef XLIBRARY_APPEARANCE_H
#define XLIBRARY_APPEARANCE_H

#include <QIcon>
#include <QPalette>
#include <QSettings>

class QPainter;

namespace appearance {
QIcon icon(const QString &name, const QColor &color = {});
QIcon logo();
void apply(const QSettings &settings, const QString &additional_style = {});
bool is_dark(const QSettings &settings);
int scaled(const QSettings &settings, int size);
void paint_selection(QPainter &painter, const QRectF &rect, const QPalette &palette, qreal radius = 8,
                     bool focused = false);
}

#endif
