#ifndef XLIBRARY_SEARCH_OVERLAY_H
#define XLIBRARY_SEARCH_OVERLAY_H

#include <QWidget>

class DesktopService;
class QGraphicsOpacityEffect;
class QLineEdit;
class QToolButton;
class QTimer;
class QVariantAnimation;
class SearchResults;
class NotificationBanner;

class SearchOverlay : public QWidget {
    Q_OBJECT
    DesktopService &service;
    QWidget *contents;
    QLineEdit *input;
    QToolButton *open_button;
    SearchResults *results;
    NotificationBanner *notice;
    QGraphicsOpacityEffect *opacity;
    QVariantAnimation *reveal;
    QVariantAnimation *height_animation;
    QTimer *activation_check;
    bool dismiss_on_deactivate = false;
    bool activation_notice = false;
    qreal progress = 1;
    qreal surface_height = 0;
    int header_height = 0;
    int inset = 16;
    void update_surface();
    void update_height();
    void refresh_appearance();
    void position_on_screen();
    void focus_search();

protected:
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

public:
    explicit SearchOverlay(DesktopService &service, QWidget *parent = nullptr);
    void toggle();
    void show_search();

signals:
    void open_library(const QString &query);
};

#endif
