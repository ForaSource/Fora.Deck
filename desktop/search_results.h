#ifndef XLIBRARY_SEARCH_RESULTS_H
#define XLIBRARY_SEARCH_RESULTS_H

#include <atomic>
#include <memory>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QListView>

#include "library_model.h"

class DesktopService;
class SearchResultModel;
struct search_entry {
    enum class kind { application, file, directory, calculation, message };
    kind type = kind::file;
    QString title;
    QString path;
    qint64 application_id = 0;
};

class SearchResults : public QListView {
    Q_OBJECT
    DesktopService &service;
    QLineEdit &input;
    bool compact;
    SearchResultModel *results;
    QFutureWatcher<QVector<search_entry>> path_watcher;
    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(0);
    QString pending_path;
    QString active_path;
    quint64 active_generation = 0;
    bool special = false;
    bool path_busy = false;
    quint64 pending_launch = 0;
    void update_query();
    void update_applications();
    void read_path();
    void replace_entries(QVector<search_entry> entries);
    void activate_current();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

public:
    SearchResults(DesktopService &service, QLineEdit &input, bool compact, QWidget *parent = nullptr);
    ~SearchResults() override;
    bool handles_query() const { return special; }
    void reset_launch() { pending_launch = 0; }

signals:
    void height_changed();
    void application_activated();
    void dismissed();
};

#endif
