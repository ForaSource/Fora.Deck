#include <cmath>
#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleHints>

#include "appearance.h"
#include "WindowManager.h"

QIcon appearance::icon(const QString &name, const QColor &requested_color) {
    const auto color = requested_color.isValid() ? requested_color : qApp->palette().color(QPalette::PlaceholderText);
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(64.0 / 24, 64.0 / 24);
    painter.setPen(QPen(color, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == "add") {
        painter.drawLine(QPointF(12, 5), QPointF(12, 19));
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
    } else if (name == "lock" || name == "private") {
        painter.drawRoundedRect(QRectF(5, 10, 14, 11), 2, 2);
        painter.drawArc(QRectF(8, 3, 8, 13), 0, 180 * 16);
        painter.drawLine(QPointF(12, 14), QPointF(12, 17));
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
    } else if (name == "favorite" || name == "favorite_filled") {
        if (name == "favorite_filled")
            painter.setBrush(color);
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
    } else if (name == "keyboard") {
        painter.drawRoundedRect(QRectF(2, 5, 20, 14), 3, 3);
        for (int y : {9, 12})
            for (int x : {6, 10, 14, 18})
                painter.drawPoint(QPointF(x, y));
        painter.drawLine(QPointF(7, 16), QPointF(17, 16));
    } else if (name == "palette") {
        QPainterPath path;
        path.moveTo(21, 11);
        path.cubicTo(21, 1, 3, 0, 3, 12);
        path.cubicTo(3, 19, 9, 22, 13, 21);
        path.cubicTo(18, 20, 12, 16, 16, 14);
        path.cubicTo(18, 13, 21, 15, 21, 11);
        painter.drawPath(path);
        for (const auto &point : {QPointF(7, 10), QPointF(10, 6), QPointF(15, 6), QPointF(18, 10)})
            painter.drawEllipse(point, 0.8, 0.8);
    } else if (name == "profile") {
        painter.drawEllipse(QRectF(8, 3, 8, 8));
        painter.drawArc(QRectF(4, 13, 16, 14), 0, 180 * 16);
        painter.drawLine(QPointF(4, 20), QPointF(20, 20));
    } else if (name == "database") {
        painter.drawEllipse(QRectF(4, 3, 16, 6));
        painter.drawLine(QPointF(4, 6), QPointF(4, 18));
        painter.drawLine(QPointF(20, 6), QPointF(20, 18));
        painter.drawArc(QRectF(4, 9, 16, 6), 180 * 16, 180 * 16);
        painter.drawArc(QRectF(4, 15, 16, 6), 180 * 16, 180 * 16);
    } else if (name == "plugin") {
        painter.drawRoundedRect(QRectF(5, 7, 14, 10), 3, 3);
        painter.drawLine(QPointF(9, 3), QPointF(9, 7));
        painter.drawLine(QPointF(15, 3), QPointF(15, 7));
        painter.drawPolyline(QPolygonF{QPointF(12, 17), QPointF(12, 21), QPointF(17, 21)});
    } else if (name == "help") {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        QPainterPath path;
        path.moveTo(9, 8);
        path.cubicTo(9, 4, 18, 6, 14, 11);
        path.lineTo(12, 13);
        painter.drawPath(path);
        painter.drawPoint(QPointF(12, 17));
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
    } else if (name == "edit") {
        painter.drawPolygon(QPolygonF{QPointF(4, 15), QPointF(16, 3), QPointF(21, 8), QPointF(9, 20), QPointF(3, 21)});
        painter.drawLine(QPointF(13, 6), QPointF(18, 11));
    } else if (name == "undo") {
        painter.drawPolyline(QPolygonF{QPointF(8, 4), QPointF(3, 9), QPointF(8, 14)});
        QPainterPath path;
        path.moveTo(3, 9);
        path.lineTo(14, 9);
        path.cubicTo(23, 9, 23, 21, 14, 21);
        painter.drawPath(path);
    } else if (name == "tag") {
        painter.drawPolygon(QPolygonF{QPointF(3, 3), QPointF(11, 3), QPointF(21, 13), QPointF(13, 21), QPointF(3, 11)});
        painter.drawEllipse(QPointF(7, 7), 1, 1);
    } else if (name == "sort") {
        painter.drawLine(QPointF(4, 6), QPointF(20, 6));
        painter.drawLine(QPointF(4, 12), QPointF(15, 12));
        painter.drawLine(QPointF(4, 18), QPointF(10, 18));
    } else if (name == "scan") {
        painter.drawPolyline(
            QPolygonF{QPointF(11, 20), QPointF(3, 20), QPointF(3, 4), QPointF(21, 4), QPointF(21, 10)});
        painter.drawLine(QPointF(3, 8), QPointF(21, 8));
        painter.drawEllipse(QPointF(16, 16), 4, 4);
        painter.drawLine(QPointF(19, 19), QPointF(22, 22));
    } else if (name == "stop") {
        painter.drawRoundedRect(QRectF(5, 5, 14, 14), 1, 1);
    } else if (name == "calculator") {
        painter.drawRoundedRect(QRectF(5, 2, 14, 20), 2, 2);
        painter.drawRect(QRectF(8, 5, 8, 4));
        for (int y : {13, 17})
            for (int x : {8, 12, 16})
                painter.drawPoint(QPointF(x, y));
    } else if (name == "new_instance") {
        painter.drawPolyline(
            QPolygonF{QPointF(12, 20), QPointF(3, 20), QPointF(3, 4), QPointF(21, 4), QPointF(21, 12)});
        painter.drawLine(QPointF(3, 8), QPointF(21, 8));
        painter.drawLine(QPointF(18, 14), QPointF(18, 22));
        painter.drawLine(QPointF(14, 18), QPointF(22, 18));
    } else if (name == "warning") {
        painter.drawPolygon(QPolygonF{QPointF(12, 3), QPointF(22, 21), QPointF(2, 21)});
        painter.drawLine(QPointF(12, 9), QPointF(12, 14));
        painter.drawPoint(QPointF(12, 17));
    } else if (name == "app") {
        painter.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
        painter.drawLine(QPointF(3, 8), QPointF(21, 8));
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

bool appearance::is_dark(const QSettings &settings) {
    WindowManager manager;
    manager.update_theme(settings.value("appearance/theme", "dark").toString(),
                         qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light);
    return manager.get_theme();
}

int appearance::scaled(const QSettings &settings, const int size) {
    return qRound(size * qBound(100, settings.value("appearance/text_scale", 100).toInt(), 200) / 100.0);
}

void appearance::paint_selection(QPainter &painter, const QRectF &rect, const QPalette &palette, const qreal radius,
                                 const bool focused) {
    painter.setBrush(palette.color(QPalette::Highlight));
    painter.setPen(QPen(palette.color(focused ? QPalette::Link : QPalette::Midlight), focused ? 2 : 1));
    painter.drawRoundedRect(rect, radius, radius);
}

void appearance::apply(const QSettings &settings, const QString &additional_style) {
    qApp->setProperty("xlibrary_disable_animations", !settings.value("appearance/animations", true).toBool());
    const bool dark = is_dark(settings);
    const QColor base(dark ? "#202328" : "#FFFFFF");
    const QColor panel(dark ? "#181A1D" : "#F5F6F8");
    const QColor field(dark ? "#2A2E34" : "#EDF0F4");
    const QColor text(dark ? "#F2F3F5" : "#20242B");
    const QColor secondary(dark ? "#AEB5BF" : "#56606D");
    const QColor border(dark ? "#343A43" : "#DEE3E9");
    const QColor selection(dark ? "#392B29" : "#FBE8E2");
    const QColor selection_top(dark ? "#2D323A" : "#E5EAF0");
    const QColor selection_border(dark ? "#F16647" : "#B63F2B");
    QPalette palette;
    palette.setColor(QPalette::Window, panel);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, selection_top);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, field);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Mid, border);
    palette.setColor(QPalette::Dark, QColor(dark ? "#778191" : "#7A8595"));
    palette.setColor(QPalette::Link, selection_border);
    palette.setColor(QPalette::Highlight, selection);
    palette.setColor(QPalette::Light, selection_top);
    palette.setColor(QPalette::Midlight, selection_border);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::PlaceholderText, secondary);
    palette.setColor(QPalette::Disabled, QPalette::Text, secondary);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, secondary);
    qApp->setPalette(palette);
    static const bool fonts_loaded = [] {
        bool loaded = true;
        for (const auto *path : {":/assets/fonts/inter-regular.ttf", ":/assets/fonts/inter-medium.ttf",
                                 ":/assets/fonts/inter-semibold.ttf"})
            loaded = (QFontDatabase::addApplicationFont(QString::fromLatin1(path)) >= 0) && loaded;
        return loaded;
    }();
    QFont font(fonts_loaded ? "Inter" : "sans-serif");
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(scaled(settings, 14));
    qApp->setFont(font);
    QString style = R"(
        QWidget { color: %1; font-family: "@font_family@"; font-size: %11px; }
        QMainWindow { background: %3; }
        QDialog { background: %2; }
        QFrame#shell, QSplitter#library_splitter { background: %3; border: none; }
        QFrame#library_workspace { background: %2; border: 1px solid %6; border-radius: 12px; }
        QFrame[panel="true"], QWidget[panel="true"] { background: %3; }
        QFrame#navigation, QScrollArea#details { background: %3; border: none; }
        QLabel[muted="true"] { color: %4; }
        QLabel[status_kind="running"] { color: @success@; }
        QLabel[status_kind="warning"] { color: @warning@; }
        QLabel[heading="true"] { font-size: %8px; font-weight: 600; }
        QLabel[subheading="true"] { font-size: %9px; font-weight: 600; }
        QLabel[application_title="true"] { font-size: %12px; font-weight: 600; }
        QLabel[brand="true"] { font-size: %13px; font-weight: 600; }
        QLabel[caption="true"] { color: %4; font-size: %14px; }
        QPushButton, QToolButton { background: %5; border: 2px solid %6; border-radius: 8px; padding: 6px 10px; min-height: %10px; }
        QPushButton:hover, QToolButton:hover { background: %17; }
        QPushButton:pressed { background: %3; }
        QPushButton:checked, QToolButton:checked { background: %7; border-color: %16; }
        QPushButton[primary="true"] { background: #F16647; color: #181A1D; border-color: transparent; font-weight: 600; min-height: @launch_height@px; }
        QPushButton[primary="true"]:hover { background: #FF795C; }
        QPushButton[primary="true"]:pressed { background: #E45E40; }
        QPushButton[primary="true"]:disabled { color: %4; background: %5; }
        QPushButton:disabled, QToolButton:disabled { color: %4; background: %3; }
        QPushButton[danger="true"] { color: %15; }
        QPushButton[danger="true"]:hover { border-color: %15; }
        QPushButton[quiet="true"] { background: transparent; border-color: transparent; text-align: left; }
        QPushButton[quiet="true"]:hover { background: %17; }
        QPushButton[quiet="true"]:checked { background: %7; border-color: transparent; }
        QPushButton[navigation="true"] { background: transparent; border: 2px solid transparent; border-radius: 8px; padding: 8px 10px; min-height: @navigation_height@px; text-align: left; }
        QPushButton[navigation="true"]:hover { background: %17; }
        QPushButton[navigation="true"]:checked { background: %7; border-color: transparent; }
        QPushButton[navigation="true"]:checked:focus, QPushButton[quiet="true"]:checked:focus { border-color: %16; }
        QLineEdit, QComboBox, QSpinBox, QKeySequenceEdit { background: %5; border: 2px solid @input_border@; border-radius: 8px; padding: 6px 8px; min-height: %10px; selection-background-color: %7; selection-color: %1; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QKeySequenceEdit:focus, QPushButton:focus, QToolButton:focus { border-color: %16; }
        QLineEdit[invalid="true"] { border-color: %15; }
        QLineEdit#search_input { min-height: @launch_height@px; }
        QLineEdit QToolButton { background: transparent; border: none; border-radius: 0; padding: 0; min-height: 0; }
        QComboBox::drop-down { border: none; width: 24px; }
        QTabBar#shortcut_tabs::tab { background: transparent; color: %4; border: 1px solid transparent; border-radius: 8px; padding: 8px 12px; margin-right: 8px; min-height: %10px; }
        QTabBar#shortcut_tabs::tab:last { margin-right: 0; }
        QTabBar#shortcut_tabs::tab:hover { background: %5; border-color: %6; }
        QTabBar#shortcut_tabs::tab:selected { color: %1; background: %7; border-color: %16; }
        QTabBar#shortcut_tabs::scroller { width: 2em; }
        QTabBar#shortcut_tabs QToolButton { padding: 0; min-width: 0; min-height: 0; border-radius: 6px; }
        QAbstractItemView { background: %2; border: none; outline: none; selection-background-color: %7; }
        QAbstractItemView::item { padding: 6px; border: 2px solid transparent; border-radius: 8px; }
        QAbstractItemView::item:hover { background: %17; }
        QAbstractItemView::item:selected { background: %7; color: %1; }
        QAbstractItemView::item:focus { border-color: %16; }
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
        QMenu { background: %2; border: 1px solid %6; border-radius: 12px; padding: 6px; }
        QMenu::item { padding: 8px 24px 8px 12px; border: 1px solid transparent; border-radius: 10px; }
        QMenu::item:selected { background: %7; border-color: %16; }
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
        QToolButton#notification_dismiss:focus { border-color: %16; }
        QProgressBar#notification_progress { background: %3; border: none; border-radius: 2px; }
        QProgressBar#notification_progress::chunk { background: #F16647; border-radius: 2px; }
        QFrame#discovery_panel { background: %3; border: 1px solid %6; border-radius: 12px; }
        QLabel#discovery_percentage { color: %1; font-weight: 600; }
        QProgressBar#discovery_bar { background: %5; border: none; border-radius: 4px; }
        QProgressBar#discovery_bar::chunk { background: #F16647; border-radius: 4px; }
        QSplitter::handle { background: %3; }
        QSplitter::handle:hover, QSplitter::handle:pressed { background: %16; }
        QSplitter#library_splitter::handle { background: transparent; border: none; margin: 14px 2px; border-radius: 1px; }
        QSplitter#library_splitter::handle:hover { background: %16; }
        QSplitter#library_splitter::handle:pressed { background: %4; }
        QGroupBox { border: none; margin-top: 24px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; }
    )";
    style = style.arg(text.name(), base.name(), panel.name(), secondary.name(), field.name(), border.name(),
                      selection.name(), QString::number(scaled(settings, 24)), QString::number(scaled(settings, 16)),
                      QString::number(scaled(settings, 36) - 16), QString::number(scaled(settings, 14)),
                      QString::number(scaled(settings, 20)), QString::number(scaled(settings, 20)),
                      QString::number(scaled(settings, 12)), dark ? "#ED9398" : "#B73548", selection_border.name(),
                      selection_top.name(), dark ? "#555960" : "#B3BAC4", dark ? "#7B8491" : "#8994A3");
    style.replace("@font_family@", font.family());
    style.replace("@input_border@", palette.color(QPalette::Dark).name());
    style.replace("@success@", dark ? "#7CCDA6" : "#237A55");
    style.replace("@warning@", dark ? "#E7BD6A" : "#8A5D00");
    style.replace("@launch_height@", QString::number(scaled(settings, 44) - 16));
    style.replace("@navigation_height@", QString::number(scaled(settings, 40) - 20));
    qApp->setStyleSheet(style + additional_style);
}
