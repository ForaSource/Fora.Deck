#ifndef XLIBRARY_UI_HELPERS_H
#define XLIBRARY_UI_HELPERS_H

#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "appearance.h"
#include "localization.h"
#include "PlatformIntegration.h"
#include "UiRegistry.h"

namespace ui_helpers {
inline QList<QKeySequence> shortcut_sequences(const QKeySequence &sequence) {
    if (sequence.isEmpty())
        return {};
    QList<QKeySequence> result{sequence};
    if (sequence.count() == 1) {
        const auto combination = sequence[0];
        if (combination.key() == Qt::Key_Return || combination.key() == Qt::Key_Enter)
            result.append(
                QKeySequence(QKeyCombination(combination.keyboardModifiers(),
                                             combination.key() == Qt::Key_Return ? Qt::Key_Enter : Qt::Key_Return)));
    }
    return result;
}

class AnimatedReveal : public QWidget {
protected:
    QWidget *contents;

private:
    QVariantAnimation *animation;
    int spacing;
    int displayed_height = 0;
    bool expanded = false;

    int preferred_height() const {
        const auto height =
            contents->hasHeightForWidth() ? contents->heightForWidth(width()) : contents->sizeHint().height();
        return qMax(height, contents->minimumSizeHint().height()) + spacing;
    }

    void update_height() {
        const auto target = expanded ? preferred_height() : 0;
        if (animation->state() == QAbstractAnimation::Running && animation->endValue().toInt() == target)
            return;
        animation->stop();
        if (!window()->isVisible() || !desktop_platform::animations_enabled() || target == displayed_height) {
            displayed_height = target;
            updateGeometry();
            if (!expanded)
                hide();
            return;
        }
        animation->setStartValue(displayed_height);
        animation->setEndValue(target);
        animation->start();
    }

protected:
    bool eventFilter(QObject *const object, QEvent *const event) override {
        if (object == contents && event->type() == QEvent::LayoutRequest)
            update_height();
        return QWidget::eventFilter(object, event);
    }

    void resizeEvent(QResizeEvent *const event) override {
        QWidget::resizeEvent(event);
        // Content retains its full height while the parent reveals it smoothly without compressing the text.
        contents->setGeometry(0, spacing, width(), preferred_height() - spacing);
        update_height();
    }

public:
    explicit AnimatedReveal(QWidget *const contents, const int spacing = 0, QWidget *const parent = nullptr)
        : QWidget(parent), contents(contents), animation(new QVariantAnimation(this)), spacing(spacing) {
        contents->setParent(this);
        contents->installEventFilter(this);
        contents->show();
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        animation->setDuration(180);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            displayed_height = value.toInt();
            updateGeometry();
        });
        connect(animation, &QVariantAnimation::finished, this, [this] {
            if (!expanded)
                hide();
        });
        hide();
    }

    QSize sizeHint() const override { return {contents->sizeHint().width(), displayed_height}; }
    QSize minimumSizeHint() const override { return {contents->minimumSizeHint().width(), 0}; }

    void set_expanded(const bool value) {
        expanded = value;
        if (expanded) {
            contents->ensurePolished();
            show();
        }
        update_height();
    }
};

class TransitionOverlay : public QWidget {
public:
    QPixmap snapshot;
    qreal opacity = 1.0;

    explicit TransitionOverlay(QWidget *const parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        // An opaque background preserves the surface color during transitions.
        painter.fillRect(rect(), palette().window());
        painter.setOpacity(opacity);
        painter.drawPixmap(rect(), snapshot);
    }
};

class PageTransition : public QWidget {
private:
    QWidget *contents;
    TransitionOverlay *overlay;
    QVariantAnimation *animation;

protected:
    void resizeEvent(QResizeEvent *const event) override {
        animation->stop();
        overlay->hide();
        overlay->snapshot = QPixmap{};
        QWidget::resizeEvent(event);
    }

public:
    explicit PageTransition(QWidget *const contents)
        : contents(contents), overlay(new TransitionOverlay(this)), animation(new QVariantAnimation(this)) {
        setAutoFillBackground(true);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(contents);
        animation->setDuration(140);
        animation->setStartValue(0.88);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            overlay->opacity = value.toReal();
            overlay->update();
        });
        connect(animation, &QVariantAnimation::finished, this, [this] {
            overlay->hide();
            overlay->snapshot = QPixmap{};
        });
    }

    void start() {
        animation->stop();
        overlay->hide();
        overlay->snapshot = QPixmap{};
        if (!contents->isVisible() || !desktop_platform::animations_enabled())
            return;
        overlay->snapshot = contents->grab();
        overlay->setGeometry(rect());
        overlay->opacity = animation->startValue().toReal();
        overlay->show();
        overlay->raise();
        animation->start();
    }
};

inline QLabel *label(const QString &text, const char *const property = nullptr) {
    auto *result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    if (property)
        result->setProperty(property, true);
    return result;
}
inline QPushButton *button(const QString &text, const char *const property = nullptr) {
    auto *result = new QPushButton(text);
    if (property)
        result->setProperty(property, true);
    return result;
}
inline QPushButton *action_button(QAction *const action, const bool quiet = false, const QString &text = {}) {
    auto *result = button(text.isNull() ? action->text() : text, quiet ? "quiet" : nullptr);
    result->setIcon(action->icon());
    result->setEnabled(action->isEnabled());
    result->setToolTip(action->toolTip());
    QObject::connect(result, &QPushButton::clicked, action, &QAction::trigger);
    QObject::connect(action, &QAction::changed, result, [action, result, text] {
        result->setText(text.isNull() ? action->text() : text);
        result->setEnabled(action->isEnabled());
        result->setIcon(action->icon());
        result->setToolTip(action->toolTip());
    });
    return result;
}
inline QScrollArea *scroll(QWidget *const contents) {
    auto *result = new QScrollArea;
    result->setWidgetResizable(true);
    result->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    result->setWidget(contents);
    return result;
}
inline void field(QVBoxLayout *const layout, const QString &name, QWidget *const control) {
    auto *caption = label(name, "muted");
    caption->setBuddy(control);
    if (control->accessibleName().isEmpty())
        control->setAccessibleName(name);
    layout->addWidget(caption);
    layout->addWidget(control);
    layout->addSpacing(8);
}
inline void prepare_dialog(QDialog *const dialog, const ui_context &context, const QSize &preferred,
                           const QSize &minimum = QSize(360, 240)) {
    const auto available = dialog->screen()->availableGeometry().size() - QSize(32, 64);
    dialog->setSizeGripEnabled(true);
    dialog->setMinimumSize(QSize(appearance::scaled(*context.settings, minimum.width()),
                                 appearance::scaled(*context.settings, minimum.height()))
                               .boundedTo(available));
    dialog->resize(QSize(appearance::scaled(*context.settings, preferred.width()),
                         appearance::scaled(*context.settings, preferred.height()))
                       .boundedTo(available));
}
inline bool confirm(QWidget *const parent, const QString &title, const QString &text,
                    const QString &accept_text = QCoreApplication::translate("XLibrary", "Remove")) {
    QMessageBox dialog(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::Cancel, parent);
    dialog.setTextFormat(Qt::PlainText);
    dialog.button(QMessageBox::Yes)->setText(accept_text);
    dialog.button(QMessageBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.setEscapeButton(QMessageBox::Cancel);
    return dialog.exec() == QMessageBox::Yes;
}
inline void open_dialog(const QString &id, const ui_context &context) {
    if (auto *existing = context.window->findChild<QWidget *>(id, Qt::FindDirectChildrenOnly)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *widget = context.registry->create(id, context, context.window);
    if (auto *dialog = qobject_cast<QDialog *>(widget)) {
        // This limit also applies to dialogs replaced by plugins.
        const auto available = dialog->screen()->availableGeometry().size() - QSize(32, 64);
        const auto size_key = "dialogs/" + id + "/size";
        const auto saved = context.settings->value(size_key).toSize();
        if (saved.isValid() && !saved.isEmpty())
            dialog->resize(appearance::scaled(*context.settings, saved.width()),
                           appearance::scaled(*context.settings, saved.height()));
        dialog->setMinimumSize(dialog->minimumSize().boundedTo(available));
        dialog->resize(dialog->size().boundedTo(available));
        dialog->setSizeGripEnabled(true);
        QObject::connect(dialog, &QDialog::finished, dialog, [dialog, settings = context.settings, size_key] {
            if (settings->property("profile_locked").toBool())
                return;
            const auto scale = appearance::scaled(*settings, 100) / 100.0;
            settings->setValue(size_key, QSize(qRound(dialog->width() / scale), qRound(dialog->height() / scale)));
        });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->show();
    } else {
        widget->setWindowFlag(Qt::Window, true);
        widget->setAttribute(Qt::WA_DeleteOnClose);
        widget->show();
    }
}
}

#endif
