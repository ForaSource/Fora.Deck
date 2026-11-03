#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

#include "appearance.h"
#include "LibrarySplitter.h"
#include "localization.h"

LibrarySplitter::LibrarySplitter(QSettings &settings, QWidget *const navigation, QWidget *const center,
                                 QWidget *const details, QWidget *const parent, std::function<bool()> can_save_layout)
    : QSplitter(Qt::Horizontal, parent), settings(settings), can_save_layout(std::move(can_save_layout)) {
    setObjectName("library_splitter");
    setChildrenCollapsible(false);
    setHandleWidth(6);
    addWidget(navigation);
    addWidget(center);
    addWidget(details);
    setStretchFactor(0, 0);
    setStretchFactor(1, 1);
    setStretchFactor(2, 0);
    handle(1)->setToolTip(QCoreApplication::translate("XLibrary", "Drag to resize the navigation pane"));
    handle(1)->setAccessibleName(QCoreApplication::translate("XLibrary", "Navigation width"));
    handle(2)->setToolTip(QCoreApplication::translate("XLibrary", "Drag to resize the details pane"));
    handle(2)->setAccessibleName(QCoreApplication::translate("XLibrary", "Details width"));
    connect(this, &QSplitter::splitterMoved, this, [this] {
        if (applying_layout || (this->can_save_layout && !this->can_save_layout()))
            return;
        const auto scale = appearance::scaled(this->settings, 100) / 100.0;
        this->settings.setValue("window/navigation_width", qRound(widget(0)->width() / scale));
        if (widget(2)->isVisible())
            this->settings.setValue("window/details_width", qRound(widget(2)->width() / scale));
    });
}

void LibrarySplitter::update_layout(const bool show_details) {
    details_requested = show_details;
    if (applying_layout || count() != 3)
        return;
    applying_layout = true;
    const auto navigation_min = qMax(appearance::scaled(settings, 176), widget(0)->minimumSizeHint().width());
    const auto center_min = qMin(appearance::scaled(settings, 280), qMax(200, width() / 2));
    auto details_min = appearance::scaled(settings, 264);
    if (const auto *details = qobject_cast<QScrollArea *>(widget(2)); details && details->widget()) {
        details_min =
            qMax(details_min, details->widget()->minimumSizeHint().width() +
                                  details->verticalScrollBar()->sizeHint().width() + details->frameWidth() * 2);
    }
    widget(0)->setMinimumWidth(navigation_min);
    widget(0)->setMaximumWidth(qMax(navigation_min, appearance::scaled(settings, 420)));
    widget(1)->setMinimumWidth(center_min);
    widget(2)->setMinimumWidth(details_min);
    widget(2)->setMaximumWidth(qMax(details_min, appearance::scaled(settings, 640)));
    const bool visible = details_requested && width() >= navigation_min + center_min + details_min + 80;
    widget(2)->setVisible(visible);
    const auto navigation_width =
        qMax(navigation_min,
             appearance::scaled(settings, qBound(176, settings.value("window/navigation_width", 208).toInt(), 420)));
    const auto details_width =
        visible
            ? qMax(details_min,
                   appearance::scaled(settings, qBound(264, settings.value("window/details_width", 304).toInt(), 640)))
            : 0;
    setSizes({navigation_width, qMax(center_min, width() - navigation_width - details_width - handleWidth() * 2),
              details_width});
    applying_layout = false;
}

void LibrarySplitter::resizeEvent(QResizeEvent *const event) {
    QSplitter::resizeEvent(event);
    update_layout(details_requested);
}
