#include <QStyleOptionButton>
#include <QStylePainter>

#include "appearance.h"
#include "PlatformIntegration.h"
#include "SelectionButton.h"

SelectionButton::SelectionButton(const QString &text, QWidget *const parent)
    : QPushButton(text, parent), animation(this) {
    setProperty("navigation", true);
    setCheckable(true);
    setAutoDefault(false);
    setCursor(Qt::PointingHandCursor);
    setToolTip(text);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    animation.setDuration(140);
    animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        selection_opacity = value.toReal();
        update();
    });
    connect(this, &QPushButton::toggled, this, [this](const bool checked) {
        animation.stop();
        if (!isVisible() || !desktop_platform::animations_enabled()) {
            selection_opacity = checked ? 1 : 0;
            update();
            return;
        }
        animation.setStartValue(selection_opacity);
        animation.setEndValue(checked ? 1.0 : 0.0);
        animation.start();
    });
}

void SelectionButton::paintEvent(QPaintEvent *) {
    QStyleOptionButton option;
    initStyleOption(&option);
    const auto icon_name = property("appearance_icon").toString();
    if (!icon_name.isEmpty())
        option.icon =
            appearance::icon(icon_name, option.palette.color(isChecked() ? QPalette::Link : QPalette::PlaceholderText));
    // Drawing through QStyle preserves support for plugin-provided styles.
    if (!(option.state & QStyle::State_KeyboardFocusChange))
        option.state &= ~QStyle::State_HasFocus;
    const auto state = option.state;
    QStylePainter painter(this);
    option.state &= ~QStyle::State_On;
    painter.drawControl(QStyle::CE_PushButtonBevel, option);
    if (selection_opacity > 0) {
        painter.save();
        painter.setOpacity(selection_opacity);
        option.state |= QStyle::State_On;
        painter.drawControl(QStyle::CE_PushButtonBevel, option);
        painter.restore();
    }
    option.state = state;
    option.rect = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    option.text = fontMetrics().elidedText(
        text(), Qt::ElideRight,
        qMax(0, option.rect.width() - (option.icon.isNull() ? 0 : option.iconSize.width() + 8)));
    painter.drawControl(QStyle::CE_PushButtonLabel, option);
}
