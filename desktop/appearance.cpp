#include <cmath>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleHints>

#include "appearance.h"
#include "WindowManager.h"

QIcon appearance::icon(const QString &name, const QColor &color) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(64.0 / 24, 64.0 / 24);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == "add") {
        painter.drawLine(QPointF(12, 5), QPointF(12, 19));
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
    } else if (name == "search") {
        painter.drawEllipse(QRectF(4, 4, 12, 12));
        painter.drawLine(QPointF(15, 15), QPointF(21, 21));
    } else if (name == "external") {
        painter.drawPolyline(QPolygonF{QPointF(13, 3), QPointF(21, 3), QPointF(21, 11)});
        painter.drawLine(QPointF(21, 3), QPointF(10, 14));
        painter.drawPolyline(QPolygonF{QPointF(9, 5), QPointF(3, 5), QPointF(3, 21), QPointF(19, 21), QPointF(19, 15)});
    } else if (name == "file") {
        painter.drawPolyline(QPolygonF{QPointF(14, 3), QPointF(14, 9), QPointF(20, 9)});
        painter.drawPolygon(QPolygonF{QPointF(4, 3), QPointF(14, 3), QPointF(20, 9), QPointF(20, 21), QPointF(4, 21)});
    } else if (name == "library") {
        for (int x : {4, 14})
            for (int y : {4, 14})
                painter.drawRoundedRect(QRectF(x, y, 6, 6), 1, 1);
    } else if (name == "favorite") {
        QPainterPath path;
        for (int i = 0; i < 10; ++i) {
            const auto angle = (i * 36 - 90) * 3.141592653589793 / 180;
            const auto radius = i % 2 ? 4.2 : 9;
            const QPointF point(12 + radius * std::cos(angle), 12 + radius * std::sin(angle));
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        path.closeSubpath();
        painter.drawPath(path);
    } else if (name == "play") {
        painter.drawPolygon(QPolygonF{QPointF(8, 4), QPointF(20, 12), QPointF(8, 20)});
    } else if (name == "scenarios") {
        painter.drawPolygon(QPolygonF{QPointF(3, 8), QPointF(12, 3), QPointF(21, 8), QPointF(12, 13)});
        painter.drawPolyline(QPolygonF{QPointF(3, 13), QPointF(12, 18), QPointF(21, 13)});
        painter.drawPolyline(QPolygonF{QPointF(3, 17), QPointF(12, 22), QPointF(21, 17)});
    } else if (name == "folder") {
        QPainterPath path;
        path.moveTo(3, 7);
        path.lineTo(3, 19);
        path.lineTo(21, 19);
        path.lineTo(21, 7);
        path.lineTo(11, 7);
        path.lineTo(9, 4);
        path.lineTo(3, 4);
        path.closeSubpath();
        painter.drawPath(path);
    } else if (name == "copy") {
        painter.drawRoundedRect(QRectF(8, 8, 12, 13), 2, 2);
        painter.drawPolyline(QPolygonF{QPointF(5, 16), QPointF(3, 16), QPointF(3, 3), QPointF(15, 3), QPointF(15, 5)});
    } else if (name == "settings") {
        painter.drawEllipse(QRectF(5, 5, 14, 14));
        painter.drawEllipse(QRectF(9, 9, 6, 6));
        for (int i = 0; i < 8; ++i) {
            painter.save();
            painter.translate(12, 12);
            painter.rotate(i * 45);
            painter.drawLine(QPointF(0, -7), QPointF(0, -10));
            painter.restore();
        }
    } else if (name == "more") {
        painter.setBrush(color);
        for (int x : {5, 12, 19})
            painter.drawEllipse(QPointF(x, 12), 1, 1);
    } else if (name == "refresh") {
        QPainterPath path;
        path.moveTo(3, 10);
        path.cubicTo(3, 6, 6, 3, 10, 3);
        path.cubicTo(14, 3, 17, 5, 20, 8);
        path.moveTo(20, 3);
        path.lineTo(20, 8);
        path.lineTo(15, 8);
        path.moveTo(21, 14);
        path.cubicTo(21, 18, 18, 21, 14, 21);
        path.cubicTo(10, 21, 7, 19, 4, 16);
        path.moveTo(4, 21);
        path.lineTo(4, 16);
        path.lineTo(9, 16);
        painter.drawPath(path);
    } else if (name == "trash") {
        painter.drawLine(QPointF(4, 6), QPointF(20, 6));
        painter.drawPolyline(QPolygonF{QPointF(9, 6), QPointF(9, 3), QPointF(15, 3), QPointF(15, 6)});
        painter.drawPolyline(QPolygonF{QPointF(6, 6), QPointF(7, 21), QPointF(17, 21), QPointF(18, 6)});
        painter.drawLine(QPointF(10, 10), QPointF(10, 17));
        painter.drawLine(QPointF(14, 10), QPointF(14, 17));
    } else if (name == "info") {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        painter.drawPoint(QPointF(12, 8));
        painter.drawLine(QPointF(12, 11), QPointF(12, 16));
    } else if (name == "x") {
        painter.drawLine(QPointF(6, 5), QPointF(18, 19));
        painter.drawLine(QPointF(18, 5), QPointF(6, 19));
    } else {
        painter.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
        painter.drawLine(QPointF(3, 8), QPointF(21, 8));
    }
    return QIcon(pixmap);
}

QIcon appearance::logo() {
    static const QIcon result(":/assets/xlibrary.ico");
    return result;
}

bool appearance::is_dark(QSettings &settings) {
    WindowManager manager;
    manager.update_theme(settings.value("appearance/theme", "dark").toString(),
                         qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light);
    return manager.get_theme();
}

int appearance::scaled(QSettings &settings, int size) {
    return qRound(size * qBound(100, settings.value("appearance/text_scale", 100).toInt(), 200) / 100.0);
}

void appearance::paint_selection(QPainter &painter, const QRectF &rect, const QPalette &palette, qreal radius) {
    QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
    gradient.setColorAt(0, palette.color(QPalette::Light));
    gradient.setColorAt(1, palette.color(QPalette::Highlight));
    painter.setBrush(gradient);
    painter.setPen(palette.color(QPalette::Midlight));
    painter.drawRoundedRect(rect, radius, radius);
}

void appearance::apply(QSettings &settings, const QString &additional_style) {
    const bool dark = is_dark(settings);
    const QColor base(dark ? "#242528" : "#F3F3F4");
    const QColor panel(dark ? "#2A2B2E" : "#EBECEE");
    const QColor field(dark ? "#303135" : "#FFFFFF");
    const QColor text(dark ? "#EBEBED" : "#22252A");
    const QColor secondary(dark ? "#A8A9AF" : "#555B64");
    const QColor border(dark ? "#424348" : "#C4C7CC");
    const QColor selection(dark ? "#3A3C40" : "#D9DDE2");
    const QColor selection_top(dark ? "#434850" : "#E4E8ED");
    const QColor selection_border(dark ? "#59616C" : "#A7B1BF");
    QPalette palette;
    palette.setColor(QPalette::Window, base);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, panel);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, field);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Mid, border);
    palette.setColor(QPalette::Highlight, selection);
    palette.setColor(QPalette::Light, selection_top);
    palette.setColor(QPalette::Midlight, selection_border);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::PlaceholderText, secondary);
    palette.setColor(QPalette::Disabled, QPalette::Text, secondary);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, secondary);
    qApp->setPalette(palette);
    QFont font("Segoe UI");
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(scaled(settings, 14));
    qApp->setFont(font);
    QString style = R"(
        QWidget { color: %1; font-family: "Segoe UI"; font-size: %11px; }
        QMainWindow, QDialog { background: %2; }
        QFrame[panel="true"], QWidget[panel="true"] { background: %3; }
        QFrame#navigation { border: none; border-right: 1px solid %6; }
        QScrollArea#details { border: none; border-left: 1px solid %6; }
        QLabel[muted="true"] { color: %4; }
        QLabel[heading="true"] { font-size: %8px; font-weight: 600; }
        QLabel[subheading="true"] { font-size: %9px; font-weight: 600; }
        QLabel[application_title="true"] { font-size: %12px; font-weight: 600; }
        QLabel[brand="true"] { font-size: %13px; font-weight: 600; }
        QLabel[caption="true"] { color: %4; font-size: %14px; }
        QPushButton, QToolButton { background: %5; border: 1px solid %6; border-radius: 8px; padding: 8px 12px; min-height: %10px; }
        QPushButton:hover, QToolButton:hover { background: %7; }
        QPushButton:pressed { background: %3; }
        QPushButton:checked, QToolButton:checked { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %17, stop:1 %7); border-color: %16; border-radius: 10px; }
        QPushButton[primary="true"] { background: #68717C; color: white; font-weight: 600; }
        QPushButton[primary="true"]:hover { background: #778391; }
        QPushButton:disabled, QToolButton:disabled { color: %4; background: %3; }
        QPushButton[danger="true"] { color: %15; }
        QPushButton[danger="true"]:hover { border-color: %15; }
        QPushButton[quiet="true"] { background: transparent; border-color: transparent; text-align: left; }
        QPushButton[quiet="true"]:hover { background: %7; }
        QPushButton[quiet="true"]:checked { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %17, stop:1 %7); border-color: %16; }
        QPushButton[navigation="true"] { background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 9px 12px; text-align: left; }
        QPushButton[navigation="true"]:hover { background: %5; border-color: %6; }
        QPushButton[navigation="true"]:checked { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %17, stop:1 %7); border-color: %16; font-weight: 600; }
        QPushButton[navigation="true"]:focus { border-color: #AAB4C1; }
        QLineEdit, QComboBox, QSpinBox, QKeySequenceEdit { background: %5; border: 1px solid %6; border-radius: 8px; padding: 8px 10px; min-height: %10px; selection-background-color: #68717C; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QKeySequenceEdit:focus, QPushButton:focus, QToolButton:focus { border: 1px solid #AAB4C1; }
        QLineEdit[invalid="true"] { border-color: #C88888; }
        QLineEdit QToolButton { background: transparent; border: none; border-radius: 0; padding: 0; min-height: 0; }
        QComboBox::drop-down { border: none; width: 24px; }
        QTabBar#shortcut_tabs::tab { background: transparent; color: %4; border: 1px solid transparent; border-radius: 8px; padding: 8px 12px; margin-right: 8px; min-height: %10px; }
        QTabBar#shortcut_tabs::tab:last { margin-right: 0; }
        QTabBar#shortcut_tabs::tab:hover { background: %5; border-color: %6; }
        QTabBar#shortcut_tabs::tab:selected { color: %1; background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %17, stop:1 %7); border-color: %16; }
        QAbstractItemView { background: %2; border: none; outline: none; selection-background-color: %7; }
        QAbstractItemView::item { padding: 6px; border: 1px solid transparent; border-radius: 10px; }
        QAbstractItemView::item:selected { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %17, stop:1 %7); border-color: %16; color: %1; }
        QListWidget#tag_list { background: transparent; }
        QTreeView#application_list::item { padding: 0; background: transparent; border: none; }
        QTreeView#application_list::item:selected, QTreeView#application_list::item:hover { background: transparent; }
        QHeaderView::section { background: %2; color: %4; border: none; border-bottom: 1px solid %6; padding: 8px 12px; text-align: left; }
        QHeaderView::section:hover { color: %1; background: %3; }
        QScrollArea { border: none; background: transparent; }
        QScrollBar:vertical { background: transparent; border: none; width: 12px; margin: 6px 3px; }
        QScrollBar::handle:vertical { background: %18; border: none; min-height: 32px; border-radius: 3px; }
        QScrollBar::handle:vertical:hover { background: %19; }
        QScrollBar::handle:vertical:pressed { background: %4; }
        QScrollBar::add-line:vertical { background: transparent; border: none; height: 0; subcontrol-position: bottom; subcontrol-origin: margin; }
        QScrollBar::sub-line:vertical { background: transparent; border: none; height: 0; subcontrol-position: top; subcontrol-origin: margin; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QScrollBar:horizontal { background: transparent; border: none; height: 12px; margin: 3px 6px; }
        QScrollBar::handle:horizontal { background: %18; border: none; min-width: 32px; border-radius: 3px; }
        QScrollBar::handle:horizontal:hover { background: %19; }
        QScrollBar::handle:horizontal:pressed { background: %4; }
        QScrollBar::add-line:horizontal { background: transparent; border: none; width: 0; subcontrol-position: right; subcontrol-origin: margin; }
        QScrollBar::sub-line:horizontal { background: transparent; border: none; width: 0; subcontrol-position: left; subcontrol-origin: margin; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
        QScrollBar::up-arrow, QScrollBar::down-arrow, QScrollBar::left-arrow, QScrollBar::right-arrow { image: none; background: transparent; border: none; width: 0; height: 0; }
        QAbstractScrollArea::corner { background: transparent; border: none; }
        QMenu { background: %3; border: 1px solid %6; padding: 6px; }
        QMenu::item { padding: 8px 24px 8px 12px; border: 1px solid transparent; border-radius: 10px; }
        QMenu::item:selected { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %17, stop:1 %7); border-color: %16; }
        QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }
        QCheckBox { spacing: 8px; padding: 6px 0px; }
        QCheckBox::indicator { width: 18px; height: 18px; }
        QLabel[error="true"] { color: %15; }
        QFrame[card="true"] { background: %3; border: 1px solid %6; border-radius: 12px; }
        QFrame[card="true"] QLabel { background: transparent; border: none; }
        QFrame#settings_surface { background: %2; }
        QToolTip { background: %3; color: %1; border: 1px solid %6; padding: 6px; }
        QStatusBar { background: %2; color: %4; }
        QFrame#notification { background: %5; border: 1px solid %16; border-radius: 12px; }
        QToolButton#notification_dismiss { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 0; min-height: 0; }
        QToolButton#notification_dismiss:hover { background: %7; border-color: %6; }
        QToolButton#notification_dismiss:pressed { background: %3; }
        QToolButton#notification_dismiss:focus { border-color: #AAB4C1; }
        QProgressBar#notification_progress { background: %3; border: none; border-radius: 2px; }
        QProgressBar#notification_progress::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #68717C, stop:1 #AAB4C1); border-radius: 2px; }
        QFrame#discovery_panel { background: %3; border: 1px solid %6; border-radius: 12px; }
        QLabel#discovery_percentage { color: %1; font-weight: 600; }
        QProgressBar#discovery_bar { background: %5; border: none; border-radius: 4px; }
        QProgressBar#discovery_bar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #68717C, stop:1 #9AA6B4); border-radius: 4px; }
        QSplitter::handle { background: %3; }
        QSplitter::handle:hover, QSplitter::handle:pressed { background: %16; }
        QGroupBox { border: none; margin-top: 24px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; }
    )";
    style = style.arg(text.name(), base.name(), panel.name(), secondary.name(), field.name(), border.name(),
                      selection.name(), QString::number(scaled(settings, 24)), QString::number(scaled(settings, 16)),
                      QString::number(scaled(settings, 20)), QString::number(scaled(settings, 14)),
                      QString::number(scaled(settings, 20)), QString::number(scaled(settings, 18)),
                      QString::number(scaled(settings, 12)), dark ? "#C88888" : "#A34040", selection_border.name(),
                      selection_top.name(), dark ? "#555960" : "#B3BAC4", dark ? "#7B8491" : "#8994A3");
    qApp->setStyleSheet(style + additional_style);
}
