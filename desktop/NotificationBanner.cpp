#include <QHideEvent>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QShowEvent>
#include <QToolButton>

#include "NotificationBanner.h"

NotificationBanner::NotificationBanner(QSettings &settings, const int spacing, const int duration_ms,
                                       QWidget *const parent)
    : AnimatedReveal(new QFrame, spacing, parent), settings(settings), message_icon(new QLabel),
      message_label(ui_helpers::label("")), dismiss_button(new QToolButton), progress(new QProgressBar),
      countdown(new QPropertyAnimation(progress, "value", this)) {
    auto *frame = static_cast<QFrame *>(contents);
    frame->setObjectName("notification");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 10, 10, 8);
    layout->setSpacing(8);
    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    message_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse |
                                           Qt::LinksAccessibleByKeyboard);
    row->addWidget(message_icon, 0, Qt::AlignTop);
    row->addWidget(message_label, 1);
    dismiss_button->setObjectName("notification_dismiss");
    dismiss_button->setCursor(Qt::PointingHandCursor);
    dismiss_button->setAccessibleName(QCoreApplication::translate("XLibrary", "Dismiss notification"));
    dismiss_button->setToolTip(dismiss_button->accessibleName());
    row->addWidget(dismiss_button, 0, Qt::AlignTop);
    layout->addLayout(row);
    progress->setObjectName("notification_progress");
    progress->setAccessibleName(QCoreApplication::translate("XLibrary", "Notification display time"));
    progress->setRange(0, duration_ms);
    progress->setTextVisible(false);
    layout->addWidget(progress);
    countdown->setDuration(duration_ms);
    countdown->setStartValue(0);
    countdown->setEndValue(duration_ms);
    countdown->setEasingCurve(QEasingCurve::Linear);
    connect(countdown, &QPropertyAnimation::finished, this, &NotificationBanner::dismiss);
    connect(dismiss_button, &QToolButton::clicked, this, &NotificationBanner::dismiss);
    connect(message_label, &QLabel::linkActivated, this, [this] {
        if (activated)
            activated();
    });
    refresh_appearance();
}

void NotificationBanner::refresh_appearance() {
    const bool dark = appearance::is_dark(settings);
    const QColor color(dark ? "#A8A9AF" : "#555B64");
    const auto icon_size = appearance::scaled(settings, 20);
    message_icon->setPixmap(appearance::icon("info", color).pixmap(icon_size, icon_size));
    message_icon->setFixedSize(icon_size, icon_size);
    dismiss_button->setIcon(appearance::icon("x", color));
    dismiss_button->setIconSize(QSize(icon_size, icon_size));
    const auto close_size = appearance::scaled(settings, 28);
    dismiss_button->setFixedSize(close_size, close_size);
    progress->setFixedHeight(appearance::scaled(settings, 4));
    if (description.isEmpty() && link_text.isEmpty()) {
        message_label->setTextFormat(Qt::PlainText);
        message_label->setText(message);
        return;
    }
    auto text = QString("<span style=\"font-weight: 600;\">%1</span>").arg(message.toHtmlEscaped());
    if (!description.isEmpty())
        text += QString("<p style=\"margin-top: 4px; margin-bottom: 0; color: %1; font-size: %2px;\">%3</p>")
                    .arg(color.name())
                    .arg(appearance::scaled(settings, 12))
                    .arg(description.toHtmlEscaped().replace('\n', "<br>"));
    if (!link_text.isEmpty())
        text += QString("<p style=\"margin-top: 6px; margin-bottom: 0;\"><a href=\"report\" "
                        "style=\"color: %1;\">%2</a></p>")
                    .arg(dark ? "#EBEBED" : "#22252A", link_text.toHtmlEscaped());
    message_label->setTextFormat(Qt::RichText);
    message_label->setText(text);
}

void NotificationBanner::show_message(const QString &text, const QString &description, const QString &link_text,
                                      const bool auto_dismiss) {
    countdown->stop();
    message = text;
    this->description = description;
    this->link_text = link_text;
    progress->setRange(0, auto_dismiss ? countdown->duration() : 0);
    if (auto_dismiss)
        progress->setValue(0);
    refresh_appearance();
    set_expanded(true);
    if (auto_dismiss) {
        countdown->start();
        if (!isVisible())
            countdown->pause();
    }
}

void NotificationBanner::showEvent(QShowEvent *const event) {
    AnimatedReveal::showEvent(event);
    if (countdown->state() == QAbstractAnimation::Paused)
        countdown->resume();
}

void NotificationBanner::hideEvent(QHideEvent *const event) {
    if (countdown->state() == QAbstractAnimation::Running)
        countdown->pause();
    AnimatedReveal::hideEvent(event);
}

void NotificationBanner::dismiss() {
    countdown->stop();
    set_expanded(false);
}
