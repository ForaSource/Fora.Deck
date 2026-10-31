#ifndef XLIBRARY_SEARCH_QUERY_H
#define XLIBRARY_SEARCH_QUERY_H

#include <optional>
#include <QStringList>
#include <QVector>

class SearchQuery {
    struct search_term {
        QString literal;
        QString alternate;
    };
    QVector<search_term> terms;
    QStringList required_tags;
    QStringList excluded_tags;
    QString phrase;
    QString alternate_phrase;
    bool incomplete_tag = false;

public:
    explicit SearchQuery(const QString &text = {});
    int rank(const QString &name, const QString &file_name, const QStringList &tags,
             const QStringList &aliases = {}) const;
    QString tag_hint(const QStringList &known_tags) const;
    bool has_text() const { return !terms.isEmpty(); }
};

namespace search_query {
QString rewrite_tag(const QString &text, const QString &old_name, const QString &new_name);
QString switch_layout(const QString &text);
bool is_path(const QString &text);
QString expanded_path(const QString &text);
struct calculation {
    bool recognized = false;
    std::optional<double> value;
};
calculation calculate(const QString &text);
}

#endif
