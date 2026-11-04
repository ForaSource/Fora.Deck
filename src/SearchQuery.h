#ifndef XLIBRARY_SEARCH_QUERY_H
#define XLIBRARY_SEARCH_QUERY_H

#include <optional>
#include <QStringList>
#include <QVector>

class SearchQuery {
public:
    struct SearchField {
        QString text;
        QStringList words;
        QString initials;
    };
    struct SearchableData {
        QVector<SearchField> names;
        SearchField file_name;
        QStringList tags;
    };

private:
    struct search_term {
        QString literal;
        QString alternate;
        int maximum_distance = 0;
    };
    QVector<search_term> terms;
    QStringList required_tags;
    QStringList excluded_tags;
    QString phrase;
    bool incomplete_tag = false;
    bool abbreviations_enabled = false;

public:
    explicit SearchQuery(const QString &text = {});
    static SearchableData prepare(const QString &name, const QString &file_name, const QStringList &tags,
                                  const QStringList &aliases = {});
    int rank(const SearchableData &data) const;
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
