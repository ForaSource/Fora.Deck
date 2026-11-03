#ifndef XLIBRARY_LIBRARY_SPLITTER_H
#define XLIBRARY_LIBRARY_SPLITTER_H

#include <functional>
#include <QSettings>
#include <QSplitter>

class LibrarySplitter : public QSplitter {
    QSettings &settings;
    bool details_requested = true;
    bool applying_layout = false;
    std::function<bool()> can_save_layout;

protected:
    void resizeEvent(QResizeEvent *event) override;

public:
    LibrarySplitter(QSettings &settings, QWidget *navigation, QWidget *center, QWidget *details, QWidget *parent,
                    std::function<bool()> can_save_layout = {});
    void update_layout(bool show_details);
};

#endif
