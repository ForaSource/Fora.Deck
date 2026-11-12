#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

#include "appearance.h"
#include "DesktopService.h"
#include "NotificationBanner.h"
#include "PlatformIntegration.h"
#include "SearchOverlay.h"
#include "SearchResults.h"
#include "SearchTools.h"

SearchOverlay::SearchOverlay(DesktopService &service, QWidget *const parent)
    : QWidget(parent,
#ifdef Q_OS_WIN
              Qt::Tool |
#else
              Qt::Window |
#endif
                  Qt::FramelessWindowHint),
      service(service), contents(new QWidget(this)), input(new QLineEdit), open_button(new QToolButton),
      results(new SearchResults(service, *input, true)), notice(new NotificationBanner(service.settings, 0, 7000)),
      opacity(new QGraphicsOpacityEffect(contents)), reveal(new QVariantAnimation(this)),
      height_animation(new QVariantAnimation(this)), activation_check(new QTimer(this)) {
    setObjectName("search_overlay");
    setWindowTitle(QCoreApplication::translate("XLibrary", "Quick search"));
    setWindowIcon(appearance::logo());
    setAttribute(Qt::WA_TranslucentBackground);
    auto *dismiss = new QAction(this);
    dismiss->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(dismiss);
    connect(dismiss, &QAction::triggered, this, &QWidget::hide);
    contents->setObjectName("overlay_contents");
    contents->setGraphicsEffect(opacity);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    input->setObjectName("overlay_search_input");
    input->setMaxLength(static_cast<int>(catalog_limits::text_length));
    input->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search"));
    input->setAccessibleName(
        QCoreApplication::translate("XLibrary", "Search applications and files, calculate or enter a path"));
    input->setToolTip(
        QCoreApplication::translate("XLibrary", "#tag includes a tag; -#tag excludes it. All conditions apply."));
    input->setClearButtonEnabled(true);
    auto *search_icon = input->addAction(appearance::icon("search"), QLineEdit::LeadingPosition);
    search_icon->setObjectName("overlay_search_icon");
    row->addWidget(input, 1);
    open_button->setIcon(appearance::icon("external"));
    open_button->setAccessibleName(QCoreApplication::translate("XLibrary", "Open XLibrary"));
    open_button->setToolTip(open_button->accessibleName());
    open_button->setCursor(Qt::PointingHandCursor);
    row->addWidget(file_search_switch(service, contents));
    row->addWidget(open_button);
    layout->addLayout(row);
    layout->addWidget(results);
    layout->addWidget(notice);
    contents->installEventFilter(this);
    connect(&service, &DesktopService::message, notice, [this](const QString &message) {
        if (isVisible()) {
            activation_notice = false;
            notice->show_message(message);
        }
    });
    connect(open_button, &QToolButton::clicked, this, [this] {
        const auto query = input->text();
        hide();
        emit open_library(query);
    });
    connect(results, &SearchResults::height_changed, this, &SearchOverlay::update_height);
    connect(results, &SearchResults::dismissed, this, &QWidget::hide);
    connect(results, &SearchResults::application_activated, this, &QWidget::hide);
    connect(&service, &DesktopService::appearance_changed, this, &SearchOverlay::refresh_appearance);
    reveal->setDuration(200);
    reveal->setStartValue(0.0);
    reveal->setEndValue(1.0);
    // A fast launch followed by a long, quiet settle, without spring overshoot.
    reveal->setEasingCurve(QEasingCurve::OutQuint);
    connect(reveal, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        progress = value.toReal();
        update_surface();
    });
    connect(reveal, &QVariantAnimation::finished, this, [this] {
        progress = 1;
        update_surface();
    });
    activation_check->setSingleShot(true);
    activation_check->setInterval(700);
    connect(activation_check, &QTimer::timeout, this, [this] {
        if (isVisible() && !isActiveWindow() && QGuiApplication::platformName().startsWith("wayland")) {
            activation_notice = true;
            notice->show_message(QCoreApplication::translate(
                "XLibrary",
                "Wayland did not activate quick search. Click the search field or assign XLibrary --overlay "
                "in the system keyboard settings."));
        }
    });
    height_animation->setDuration(140);
    height_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(height_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        surface_height = value.toReal();
        update_surface();
    });
    connect(height_animation, &QVariantAnimation::finished, this,
            [this] { resize(width(), qMin(qRound(surface_height) + 16, screen()->availableGeometry().height())); });
    refresh_appearance();
    hide();
}

void SearchOverlay::refresh_appearance() {
    notice->refresh_appearance();
    input->findChild<QAction *>("overlay_search_icon")->setIcon(appearance::icon("search"));
    open_button->setIcon(appearance::icon("library"));
    inset = appearance::scaled(service.settings, 16);
    header_height = appearance::scaled(service.settings, 40);
    input->setFixedHeight(header_height);
    open_button->setFixedSize(appearance::scaled(service.settings, 40), appearance::scaled(service.settings, 40));
    open_button->setIconSize(QSize(appearance::scaled(service.settings, 20), appearance::scaled(service.settings, 20)));
    for (auto *control : input->findChildren<QAbstractButton *>())
        control->setIconSize(open_button->iconSize());
    auto font = input->font();
    font.setPixelSize(appearance::scaled(service.settings, 20));
    input->setFont(font);
    setStyleSheet(
        QStringLiteral(
            "QWidget#search_overlay, QWidget#overlay_contents { background: transparent; }"
            "QLineEdit#overlay_search_input { background: transparent; border: 2px solid transparent; padding: 0 6px; "
            "min-height: %1px; max-height: %1px; font-size: %3px; }"
            "QLineEdit#overlay_search_input:focus { border-color: %2; }"
            "QListView#search_results { background: transparent; border: none; }"
            "QToolButton { border: 2px solid transparent; border-radius: 8px; background: transparent; }"
            "QToolButton:focus { border-color: %2; }"
            "QToolButton:hover { background: rgba(135,140,150,35); }")
            .arg(header_height - 4)
            .arg(palette().color(QPalette::Link).name())
            .arg(appearance::scaled(service.settings, 20)));
    update_height();
}

void SearchOverlay::update_height() {
    // Resolve stylesheet constraints before measuring, including on the first show.
    contents->ensurePolished();
    contents->layout()->activate();
    const int content_height = contents->layout()->sizeHint().height();
    // The painted surface starts 8 px below the window; keep both inner insets equal.
    const qreal target = content_height + (inset - 8) * 2;
    const auto available = screen()->availableGeometry();
    const int desired_height = qMin(qRound(target) + 16, available.height());
    const bool animate =
        isVisible() && desktop_platform::animations_enabled() && progress >= 1 && surface_height != target;
    resize(width(), animate ? qMax(desired_height, height()) : desired_height);
    contents->setGeometry(inset, inset, qMax(1, width() - inset * 2), content_height);
    height_animation->stop();
    if (animate) {
        height_animation->setStartValue(surface_height);
        height_animation->setEndValue(target);
        height_animation->start();
    } else {
        surface_height = target;
        update_surface();
    }
}

void SearchOverlay::update_surface() {
    // Children keep their final size. A mask reveals them without ever stretching glyphs.
    const qreal center_y = inset + header_height / 2.0;
    const qreal surface_width = 8 + (width() - 16 - 8) * progress;
    const qreal animated_height = 8 + (surface_height - 8) * progress;
    const QRectF surface((width() - surface_width) / 2, center_y * (1 - progress) + 8 * progress - 4 * (1 - progress),
                         surface_width, animated_height);
    // Without an X11 compositor, alpha pixels outside the panel are displayed as black.
    // A native window shape removes those pixels; composited desktops keep the soft shadow.
    if (!translucent_surface) {
        QPainterPath window_clip;
        window_clip.addRoundedRect(surface, qMin(12.0, animated_height / 2), qMin(12.0, animated_height / 2));
        setMask(QRegion(window_clip.toFillPolygon().toPolygon()));
    } else if (!mask().isEmpty()) {
        clearMask();
    }
    QPainterPath clip;
    clip.addRoundedRect(surface.translated(-inset, -inset), qMin(12.0, animated_height / 2),
                        qMin(12.0, animated_height / 2));
    if (progress < 1)
        contents->setMask(QRegion(clip.toFillPolygon().toPolygon()));
    else
        contents->clearMask();
    const auto time = reveal->state() == QAbstractAnimation::Running ? reveal->currentTime() / 200.0 : 1.0;
    opacity->setOpacity(qBound(0.0, (time - 0.12) / 0.7, 1.0));
    update();
}

void SearchOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal center_y = inset + header_height / 2.0;
    const qreal surface_width = 8 + (width() - 24) * progress;
    const qreal animated_height = 8 + (surface_height - 8) * progress;
    const QRectF surface((width() - surface_width) / 2, (center_y - 4) * (1 - progress) + 8 * progress, surface_width,
                         animated_height);
    const auto radius = qMin(12.0, animated_height / 2);
    const bool dark = appearance::is_dark(service.settings);
    for (int spread = 7; spread > 0; --spread) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, (dark ? 5 : 3) * spread / 2));
        painter.drawRoundedRect(surface.adjusted(-spread, -spread / 2.0, spread, spread), radius + spread,
                                radius + spread);
    }
    painter.setBrush(palette().base());
    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawRoundedRect(surface, radius, radius);
    if (results->isVisible() && progress == 1) {
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(QPointF(inset, inset + header_height + 2),
                         QPointF(width() - inset, inset + header_height + 2));
    }
}

void SearchOverlay::toggle() {
    if (isVisible())
        hide();
    else
        show_search();
}

void SearchOverlay::position_on_screen() {
    auto *target_screen = QGuiApplication::screenAt(QCursor::pos());
    if (!target_screen)
        target_screen = QGuiApplication::primaryScreen();
    const auto available = target_screen->availableGeometry();
    setScreen(target_screen);
    resize(qMax(1, qMin(appearance::scaled(service.settings, 680), available.width() - 24)), height());
    update_height();
    move(available.center().x() - width() / 2,
         qBound(available.top(), available.top() + available.height() / 4, available.bottom() - height() + 1));
    // Create the native surface with the final bounds before mapping it.
    winId();
    windowHandle()->setGeometry(geometry());
}

void SearchOverlay::focus_search() {
    input->setFocus(Qt::ShortcutFocusReason);
    desktop_platform::activate_overlay(this);
    activation_check->start();
}

void SearchOverlay::show_search() {
    if (auto *modal = QApplication::activeModalWidget()) {
        modal->raise();
        modal->activateWindow();
        return;
    }
    reveal->stop();
    height_animation->stop();
    activation_check->stop();
    translucent_surface = desktop_platform::translucent_windows_supported();
    dismiss_on_deactivate = isVisible() && isActiveWindow();
    progress = 0;
    input->clear();
    position_on_screen();
    progress = desktop_platform::animations_enabled() ? 0 : 1;
    update_surface();
    show();
    if (progress == 0)
        reveal->start();
    focus_search();
}

bool SearchOverlay::event(QEvent *const event) {
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ApplicationFontChange)
        QTimer::singleShot(0, this, &SearchOverlay::refresh_appearance);
    if (event->type() == QEvent::WindowActivate) {
        dismiss_on_deactivate = true;
        activation_check->stop();
        if (activation_notice) {
            activation_notice = false;
            notice->dismiss();
        }
    } else if (event->type() == QEvent::WindowDeactivate && isVisible() && dismiss_on_deactivate) {
        const auto *popup = QApplication::activePopupWidget();
        const auto *modal = QApplication::activeModalWidget();
        if ((!popup || !isAncestorOf(popup)) && (!modal || !isAncestorOf(modal)))
            hide();
    }
    return QWidget::event(event);
}

bool SearchOverlay::eventFilter(QObject *const object, QEvent *const event) {
    if (object == contents && event->type() == QEvent::LayoutRequest)
        update_height();
    return QWidget::eventFilter(object, event);
}

void SearchOverlay::hideEvent(QHideEvent *const event) {
    results->reset_launch();
    reveal->stop();
    height_animation->stop();
    activation_check->stop();
    dismiss_on_deactivate = false;
    progress = 1;
    update_surface();
    notice->dismiss();
    activation_notice = false;
    QWidget::hideEvent(event);
}
