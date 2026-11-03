#ifndef XLIBRARY_SELECTION_BUTTON_H
#define XLIBRARY_SELECTION_BUTTON_H

#include <QPushButton>
#include <QVariantAnimation>

// Shared selection control for built-in groups and plugin-provided sections.
class SelectionButton : public QPushButton {
    QVariantAnimation animation;
    qreal selection_opacity = 0;

protected:
    void paintEvent(QPaintEvent *event) override;

public:
    explicit SelectionButton(const QString &text, QWidget *parent = nullptr);
};

#endif
