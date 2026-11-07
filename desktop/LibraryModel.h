#ifndef XLIBRARY_LIBRARY_MODEL_H
#define XLIBRARY_LIBRARY_MODEL_H

#include <QAbstractTableModel>
#include <QCache>
#include <QCollator>
#include <QIcon>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QThreadPool>

#include "CatalogTypes.h"
#include "SearchQuery.h"

class LibraryModel : public QAbstractTableModel {
    Q_OBJECT
private:
    QVector<app_record> records;
    QHash<qint64, int> rows_by_id;
    QVector<SearchQuery::SearchableData> searchable_records;
    QVector<QString> icon_keys;
    QVector<QString> icon_kinds;
    mutable QHash<QString, QIcon> fallback_icons;
    QStringList search_tags;
    QVector<QCollatorSortKey> name_sort_keys;
    QVector<QCollatorSortKey> tag_sort_keys;
    mutable QCache<QString, QIcon> icons{256};
    mutable QSet<QString> pending_icons;
    QThreadPool icon_pool;
    QIcon fallback_icon(const QString &kind) const;
    void request_icon(const app_record &record, const QString &key, const QString &kind) const;

public:
    enum role { id_role = Qt::UserRole + 1, record_role, tags_role, running_role, favorite_role, available_role };
    explicit LibraryModel(QObject *parent = nullptr);
    ~LibraryModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void replace(QVector<app_record> applications);
    void update_state(const QVector<app_record> &applications);
    int row_for_id(qint64 id) const { return rows_by_id.value(id, -1); }
    const app_record *find(qint64 id) const;
    const app_record &at(const int row) const { return records[row]; }
    const SearchQuery::SearchableData &search_data(const int row) const { return searchable_records[row]; }
    const QString &search_name(const int row) const { return search_data(row).names.front().text; }
    const QString &search_path(const int row) const { return search_data(row).file_name.text; }
    const QString &tag_names(const int row) const { return search_tags[row]; }
    int compare_names(int left_row, int right_row) const;
    int compare_tags(int left_row, int right_row) const;
    const QVector<app_record> &applications() const { return records; }
};

class LibraryFilter : public QSortFilterProxyModel {
    Q_OBJECT
private:
    SearchQuery query;
    QString query_text;
    mutable QVector<int> query_ranks;
    QMetaObject::Connection reset_connection;
    QMetaObject::Connection state_connection;
    int query_rank(int row) const;
    QString section = "library";
    QString selected_tag;
    int sort_mode = 0;

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

public:
    explicit LibraryFilter(QObject *parent = nullptr);
    void setSourceModel(QAbstractItemModel *model) override;
    QString text() const { return query_text; }
    int sort_mode_value() const { return sort_mode; }
    void set_query(const QString &query);
    void set_section(const QString &value, const QString &tag = {});
    void set_sort_mode(int value, Qt::SortOrder order = Qt::AscendingOrder);
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

signals:
    void query_changed(const QString &text);
    void sort_changed(int mode, Qt::SortOrder order);
};

#endif
