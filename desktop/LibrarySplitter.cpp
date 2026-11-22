#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

#include "appearance.h"
#include "LibrarySplitter.h"
#include "localization.h"

namespace {
class LibrarySplitterHandle final : public QSplitterHandle {
    const QSettings &settings;
    QPoint cursor_position;
    bool hovered = false;
    bool dragging = false;

protected:
    void enterEvent(QEnterEvent *event) override {
        hovered = true;
        cursor_position = mapFromGlobal(QCursor::pos());
        update();
        QSplitterHandle::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        hovered = false;
        update();
        QSplitterHandle::leaveEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        QSplitterHandle::mouseMoveEvent(event);
        cursor_position = mapFromGlobal(event->globalPosition().toPoint());
        update();
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton)
            dragging = true;
        cursor_position = event->position().toPoint();
        update();
        QSplitterHandle::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton)
            dragging = false;
        hovered = rect().contains(mapFromGlobal(event->globalPosition().toPoint()));
        update();
        QSplitterHandle::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override {
        if (!hovered && !dragging)
            return;
        const qreal stroke_width = appearance::scaled(settings, 2) * 19.0 / 16.0;
        const qreal spacing = appearance::scaled(settings, 4) * 19.0 / 16.0;
        const qreal outer_height = appearance::scaled(settings, 30);
        const qreal middle_height = appearance::scaled(settings, 55);
        const qreal half_height = middle_height / 2 + appearance::scaled(settings, 10);
        const qreal center_y = height() >= half_height * 2
                                   ? qBound(half_height, qreal(cursor_position.y()), height() - half_height)
                                   : height() / 2.0;
        const qreal center_x = width() / 2.0;
        const QColor color = dragging ? QColor("#F16647") : palette().color(QPalette::Text);
        QColor backdrop = color;
        backdrop.setAlpha(dragging ? 30 : 15);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(backdrop);
        painter.drawRoundedRect(QRectF(0, center_y - half_height, width(), half_height * 2), width() / 2.0,
                                width() / 2.0);
        QColor stroke = color;
        stroke.setAlpha(dragging ? 255 : 160);
        painter.setBrush(stroke);
        for (int index = -1; index <= 1; ++index) {
            const qreal bar_height = index == 0 ? middle_height : outer_height;
            painter.drawRoundedRect(QRectF(center_x + index * spacing - stroke_width / 2, center_y - bar_height / 2,
                                           stroke_width, bar_height),
                                    stroke_width / 2, stroke_width / 2);
        }
    }

public:
    LibrarySplitterHandle(const QSettings &settings, QSplitter *splitter)
        : QSplitterHandle(Qt::Horizontal, splitter), settings(settings) {
        setMouseTracking(true);
    }
};
}

QSplitterHandle *LibrarySplitter::createHandle() {
    return new LibrarySplitterHandle(settings, this);
}

LibrarySplitter::LibrarySplitter(QSettings &settings, QWidget *const navigation, QWidget *const center,
                                 QWidget *const details, QWidget *const parent, std::function<bool()> can_save_layout)
    : QSplitter(Qt::Horizontal, parent), settings(settings), can_save_layout(std::move(can_save_layout)) {
    setObjectName("library_splitter");
    setChildrenCollapsible(false);
    setHandleWidth(appearance::scaled(settings, 19));
    addWidget(navigation);
    addWidget(center);
    addWidget(details);
    setStretchFactor(0, 0);
    setStretchFactor(1, 1);
    setStretchFactor(2, 0);
    handle(1)->setToolTip(QCoreApplication::translate("Fora.Deck", "Drag to resize the navigation pane"));
    handle(1)->setAccessibleName(QCoreApplication::translate("Fora.Deck", "Navigation width"));
    handle(2)->setToolTip(QCoreApplication::translate("Fora.Deck", "Drag to resize the details pane"));
    handle(2)->setAccessibleName(QCoreApplication::translate("Fora.Deck", "Details width"));
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
    const auto center_min = qMin(appearance::scaled(settings, 400), qMax(200, width() / 2));
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
    const bool visible =
        details_requested && width() >= qMax(appearance::scaled(settings, 1180),
                                             navigation_min + center_min + details_min + handleWidth() * 2);
    widget(2)->setVisible(visible);
    const auto navigation_width =
        qMax(navigation_min,
             appearance::scaled(settings, qBound(176, settings.value("window/navigation_width", 208).toInt(), 420)));
    const auto details_width =
        visible
            ? qMax(details_min,
                   appearance::scaled(settings, qBound(264, settings.value("window/details_width", 320).toInt(), 640)))
            : 0;
    setSizes({navigation_width, qMax(center_min, width() - navigation_width - details_width - handleWidth() * 2),
              details_width});
    applying_layout = false;
}

void LibrarySplitter::resizeEvent(QResizeEvent *const event) {
    QSplitter::resizeEvent(event);
    update_layout(details_requested);
}
