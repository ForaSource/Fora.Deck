#include <algorithm>
#include <array>
#include <cmath>
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>

#include "SearchQuery.h"

QString search_query::switch_layout(const QString &text) {
    const QString latin = QStringLiteral("`qwertyuiop[]asdfghjkl;'zxcvbnm,.");
    const QString cyrillic = QStringLiteral("ёйцукенгшщзхъфывапролджэячсмитьбю");
    auto result = text.toCaseFolded();
    for (auto &character : result) {
        const auto latin_index = latin.indexOf(character);
        const auto cyrillic_index = cyrillic.indexOf(character);
        if (latin_index >= 0)
            character = cyrillic[latin_index];
        else if (cyrillic_index >= 0)
            character = latin[cyrillic_index];
    }
    return result;
}

std::optional<QRegularExpression> search_query::file_pattern(const QString &text) {
    if (!text.contains('*') && !text.contains('?'))
        return std::nullopt;
    // Escape every other regex metacharacter so filenames such as "report[1].txt" stay literal.
    auto expression = QRegularExpression::escape(text.normalized(QString::NormalizationForm_C));
    expression.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
    expression.replace(QStringLiteral("\\?"), QStringLiteral("."));
    return QRegularExpression(QRegularExpression::anchoredPattern(expression),
                              QRegularExpression::CaseInsensitiveOption |
                                  QRegularExpression::DotMatchesEverythingOption);
}

namespace {
QString normalized_search(const QString &text) {
    return text.normalized(QString::NormalizationForm_C).toCaseFolded().simplified();
}

constexpr qsizetype maximum_fuzzy_length = 64;
constexpr qsizetype maximum_fuzzy_words = 32;
// Keep category, position and tie breakers in disjoint ranges for the shared integer rank.
constexpr int rank_stride = 1 << 20;
constexpr int abbreviation_rank = 3 * rank_stride;
constexpr int fuzzy_rank = 4 * rank_stride;

int position_rank(const qsizetype position) {
    return static_cast<int>(qMin(position, qsizetype(rank_stride / 4 - 1))) * 4;
}

int substring_rank(const QString &query, const QString &alternate, const SearchQuery::SearchField &field) {
    auto best = -1;
    const auto consider = [&field, &best](const QString &text, const int layout_penalty) {
        for (auto position = field.text.indexOf(text); position >= 0;
             position = field.text.indexOf(text, position + 1)) {
            const auto end = position + text.size();
            const auto starts_word = position == 0 || !field.text[position - 1].isLetterOrNumber();
            const auto ends_word = end == field.text.size() || !field.text[end].isLetterOrNumber();
            const auto category = starts_word ? (ends_word ? 0 : 1) : 2;
            const auto rank =
                category * rank_stride + position_rank(position) + (field.text == text ? 0 : 2) + layout_penalty;
            if (best < 0 || rank < best)
                best = rank;
            if (category == 0)
                break;
        }
    };
    consider(query, 0);
    if (alternate != query)
        consider(alternate, 1);
    return best;
}

SearchQuery::SearchField prepare_field(const QString &text) {
    SearchQuery::SearchField field;
    field.text = normalized_search(text);
    qsizetype start = 0;
    while (start < field.text.size()) {
        while (start < field.text.size() && !field.text[start].isLetterOrNumber())
            ++start;
        auto end = start;
        while (end < field.text.size() && field.text[end].isLetterOrNumber())
            ++end;
        if (end > start) {
            field.words.append(field.text.mid(start, end - start));
            field.initials.append(field.text[start]);
        }
        start = end;
    }
    return field;
}

bool abbreviation_match(const QString &query, const SearchQuery::SearchField &field) {
    if (field.words.size() < 2 || query.size() < 2 || query.size() > maximum_fuzzy_length)
        return false;
    if (field.initials.startsWith(query))
        return true;
    // Every participating word contributes a prefix, so "vscode" can mean "Visual Studio Code".
    std::array<bool, maximum_fuzzy_length + 1> previous{};
    previous[0] = true;
    const auto word_count = qMin(field.words.size(), maximum_fuzzy_words);
    for (qsizetype word_index = 0; word_index < word_count; ++word_index) {
        const auto &word = field.words[word_index];
        std::array<bool, maximum_fuzzy_length + 1> current{};
        for (qsizetype position = 0; position < query.size(); ++position) {
            if (!previous[position])
                continue;
            const auto length = qMin(word.size(), query.size() - position);
            for (qsizetype offset = 0; offset < length && word[offset] == query[position + offset]; ++offset)
                current[position + offset + 1] = true;
        }
        if (word_index > 0 && current[query.size()])
            return true;
        previous = current;
    }
    return false;
}

int typo_distance(const QString &query, const QString &candidate, const int limit) {
    const auto length = query.size();
    const auto candidate_length = candidate.size();
    if (limit == 0 || candidate_length > maximum_fuzzy_length || qAbs(length - candidate_length) > limit)
        return limit + 1;
    // A narrow band and fixed-size rows bound work even for unusually long catalog names.
    std::array<int, maximum_fuzzy_length + 1> previous_previous{};
    std::array<int, maximum_fuzzy_length + 1> previous;
    previous.fill(limit + 1);
    for (qsizetype column = 0; column <= qMin(candidate_length, qsizetype(limit)); ++column)
        previous[column] = static_cast<int>(column);
    for (qsizetype row = 1; row <= length; ++row) {
        std::array<int, maximum_fuzzy_length + 1> current;
        current.fill(limit + 1);
        if (row <= limit)
            current[0] = static_cast<int>(row);
        auto best = limit + 1;
        const auto first = qMax(qsizetype(1), row - limit);
        const auto last = qMin(candidate_length, row + limit);
        for (auto column = first; column <= last; ++column) {
            const auto substitution = previous[column - 1] + (query[row - 1] != candidate[column - 1]);
            auto distance = qMin(substitution, qMin(previous[column] + 1, current[column - 1] + 1));
            if (row > 1 && column > 1 && query[row - 1] == candidate[column - 2] &&
                query[row - 2] == candidate[column - 1])
                distance = qMin(distance, previous_previous[column - 2] + 1);
            current[column] = distance;
            best = qMin(best, distance);
        }
        if (best > limit)
            return limit + 1;
        previous_previous = previous;
        previous = current;
    }
    return previous[candidate_length];
}

int fuzzy_field_rank(const QString &query, const SearchQuery::SearchField &field, const int limit) {
    auto best = typo_distance(query, field.text, limit) * rank_stride;
    const auto word_count = qMin(field.words.size(), maximum_fuzzy_words);
    qsizetype position = 0;
    for (qsizetype index = 0; index < word_count; ++index) {
        position = field.text.indexOf(field.words[index], position);
        const auto distance = typo_distance(query, field.words[index], limit);
        best = qMin(best, distance * rank_stride + position_rank(position));
        position += field.words[index].size();
    }
    return best;
}
}

SearchQuery::SearchQuery(const QString &text) {
    const auto words = normalized_search(text).split(' ', Qt::SkipEmptyParts);
    QStringList names;
    for (const auto &word : words) {
        if (word == "#" || word == "-#")
            incomplete_tag = true;
        else if (word.startsWith("-#"))
            excluded_tags.append(word.mid(2));
        else if (word.startsWith('#'))
            required_tags.append(word.mid(1));
        else {
            const auto maximum_distance = word.size() < 4 || word.size() > maximum_fuzzy_length ? 0
                                          : word.size() < 6                                     ? 1
                                                                                                : 2;
            terms.append({word, search_query::switch_layout(word), maximum_distance});
            names.append(word);
        }
    }
    phrase = names.join(' ');
    alternate_phrase = search_query::switch_layout(phrase);
    abbreviations_enabled =
        phrase.size() >= 2 && phrase.size() <= maximum_fuzzy_length &&
        std::all_of(phrase.begin(), phrase.end(), [](const QChar character) { return character.isLetterOrNumber(); });
}

SearchQuery::SearchableData SearchQuery::prepare(const QString &name, const QString &file_name, const QStringList &tags,
                                                 const QStringList &aliases) {
    SearchableData data;
    data.names.reserve(aliases.size() + 1);
    data.names.append(prepare_field(name));
    for (const auto &alias : aliases)
        data.names.append(prepare_field(alias));
    data.file_name = prepare_field(file_name);
    data.tags.reserve(tags.size());
    for (const auto &tag : tags)
        data.tags.append(normalized_search(tag));
    return data;
}

QString SearchQuery::tag_hint(const QStringList &known_tags) const {
    if (incomplete_tag)
        return QCoreApplication::translate("XLibrary", "Enter a tag name after # or -#.");
    QStringList unknown;
    for (const auto &tag : required_tags + excluded_tags) {
        bool found = false;
        for (const auto &known : known_tags)
            found |= normalized_search(known) == tag;
        if (!found && !unknown.contains(tag))
            unknown.append(tag);
    }
    return unknown.isEmpty() ? QString{}
                             : QCoreApplication::translate("XLibrary", "Unknown tags: %1").arg(unknown.join(", "));
}

QString search_query::rewrite_tag(const QString &text, const QString &old_name, const QString &new_name) {
    const QRegularExpression token("(?<!\\S)(-?#)(\\S+)", QRegularExpression::UseUnicodePropertiesOption);
    const auto previous = normalized_search(old_name);
    auto matches = token.globalMatch(text);
    QString result;
    qsizetype position = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(position, match.capturedStart() - position);
        if (normalized_search(match.captured(2)) != previous)
            result += match.captured();
        else if (!new_name.isEmpty())
            result += match.captured(1) + new_name;
        position = match.capturedEnd();
    }
    return result + text.mid(position);
}

int SearchQuery::rank(const QString &name, const QString &file_name, const QStringList &tags,
                      const QStringList &aliases) const {
    return rank(prepare(name, file_name, tags, aliases));
}

int SearchQuery::rank(const SearchableData &data, const bool suggestions) const {
    if (incomplete_tag)
        return -1;
    for (const auto &tag : required_tags)
        if (!data.tags.contains(tag))
            return -1;
    for (const auto &tag : excluded_tags)
        if (data.tags.contains(tag))
            return -1;
    if (terms.isEmpty())
        return 0;
    const auto any_field = [&data](const auto &matches) {
        return matches(data.file_name) || std::any_of(data.names.begin(), data.names.end(), matches);
    };
    if (!data.names.isEmpty()) {
        if (data.names.front().text == phrase)
            return 0;
        if (data.names.front().text == alternate_phrase)
            return 1;
    }
    int direct_rank = 0;
    bool all_direct = true;
    for (const auto &term : terms) {
        auto best = data.names.isEmpty() ? -1 : substring_rank(term.literal, term.alternate, data.names.front());
        // A filename or alias must not move an existing title match to an earlier position.
        if (best < 0) {
            const auto consider = [&term, &best](const auto &field) {
                const auto rank = substring_rank(term.literal, term.alternate, field);
                if (rank >= 0 && (best < 0 || rank < best))
                    best = rank;
            };
            consider(data.file_name);
            for (qsizetype index = 1; index < data.names.size(); ++index)
                consider(data.names[index]);
        }
        if (best < 0) {
            all_direct = false;
            break;
        }
        direct_rank = qMax(direct_rank, best);
    }
    if (all_direct)
        return direct_rank;
    if (!suggestions)
        return -1;
    if (abbreviations_enabled && any_field([this](const auto &field) {
            return abbreviation_match(phrase, field) || abbreviation_match(alternate_phrase, field);
        }))
        return abbreviation_rank;
    int suggestion_rank = 0;
    for (const auto &term : terms) {
        if (any_field([&term](const auto &field) {
                return field.text.contains(term.literal) || field.text.contains(term.alternate);
            }))
            continue;
        if (term.maximum_distance == 0)
            return -1;
        auto best = (term.maximum_distance + 1) * rank_stride;
        const auto consider = [&term, &best](const auto &field) {
            best = qMin(best, fuzzy_field_rank(term.literal, field, term.maximum_distance));
            if (term.alternate != term.literal)
                best = qMin(best, fuzzy_field_rank(term.alternate, field, term.maximum_distance) + 1);
        };
        if (!data.names.isEmpty())
            consider(data.names.front());
        const auto title_rank = best;
        consider(data.file_name);
        for (qsizetype index = 1; index < data.names.size(); ++index)
            consider(data.names[index]);
        // Prefer fewer edits in any field, then use the displayed title's position when equally close.
        if (title_rank / rank_stride == best / rank_stride)
            best = title_rank;
        if (best / rank_stride > term.maximum_distance)
            return -1;
        suggestion_rank = qMax(suggestion_rank, best);
    }
    return fuzzy_rank + suggestion_rank;
}

bool SearchQuery::accepts_rank(const int rank, const int best_rank) {
    if (rank < 0 || best_rank < 0)
        return false;
    // Typo suggestions are a fallback, limited to the closest available edit distance.
    return best_rank < fuzzy_rank ? rank < fuzzy_rank : rank / rank_stride == best_rank / rank_stride;
}

bool search_query::is_path(const QString &text) {
    auto value = text.trimmed();
    if (value.startsWith('"'))
        value.remove(0, 1);
    return value.startsWith('/') || value.startsWith('\\') || value.startsWith('~') || value.startsWith('%') ||
           value.startsWith("./") || value.startsWith("../") || value.startsWith(".\\") || value.startsWith("..\\") ||
           value.startsWith('$') || (value.size() >= 2 && value[0].isLetter() && value[1] == ':');
}

QString search_query::expanded_path(const QString &text) {
    auto value = text.trimmed();
    if (value.startsWith('"'))
        value.remove(0, 1);
    if (value.endsWith('"'))
        value.chop(1);
    if (value == "~" || value.startsWith("~/") || value.startsWith("~\\"))
        value.replace(0, 1, QDir::homePath());
    const QRegularExpression variable(
        "%([A-Za-z_][A-Za-z_0-9]*)%|\\$\\{([A-Za-z_][A-Za-z_0-9]*)\\}|\\$([A-Za-z_][A-Za-z_0-9]*)");
    auto matches = variable.globalMatch(value);
    QString result;
    qsizetype position = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += value.mid(position, match.capturedStart() - position);
        auto name = match.captured(1) + match.captured(2) + match.captured(3);
        result += qEnvironmentVariableIsSet(name.toLatin1().constData())
                      ? qEnvironmentVariable(name.toLatin1().constData())
                      : match.captured();
        position = match.capturedEnd();
    }
    result += value.mid(position);
    return QDir::fromNativeSeparators(result);
}

namespace {
class Calculator {
    QString text;
    qsizetype position = 0;
    int depth = 0;
    bool valid = true;

    void spaces() {
        while (position < text.size() && text[position].isSpace())
            ++position;
    }
    bool take(const QChar character) {
        spaces();
        if (position < text.size() && text[position] == character) {
            ++position;
            return true;
        }
        return false;
    }
    double expression() {
        auto value = product();
        while (valid) {
            if (take('+'))
                value += product();
            else if (take('-'))
                value -= product();
            else
                break;
            valid &= std::isfinite(value);
        }
        return value;
    }
    double product() {
        auto value = unary();
        while (valid) {
            if (take('*'))
                value *= unary();
            else if (take('/')) {
                const auto divisor = unary();
                valid &= divisor != 0;
                if (valid)
                    value /= divisor;
            } else if (take('%')) {
                const auto divisor = unary();
                valid &= divisor != 0;
                if (valid)
                    value = std::fmod(value, divisor);
            } else
                break;
            valid &= std::isfinite(value);
        }
        return value;
    }
    double unary() {
        if (++depth > 64) {
            valid = false;
            --depth;
            return 0;
        }
        double value;
        if (take('+'))
            value = unary();
        else if (take('-'))
            value = -unary();
        else if (take(QChar(0x221a)))
            value = std::sqrt(unary());
        else {
            value = primary();
            valid &= std::isfinite(value);
            if (take('^'))
                value = std::pow(value, unary());
        }
        valid &= std::isfinite(value);
        --depth;
        return value;
    }
    double primary() {
        spaces();
        if (text.mid(position, 4) == "sqrt") {
            position += 4;
            if (!take('(')) {
                valid = false;
                return 0;
            }
            const auto value = expression();
            valid &= take(')');
            return std::sqrt(value);
        }
        if (take('(')) {
            const auto value = expression();
            valid &= take(')');
            return value;
        }
        spaces();
        const auto start = position;
        while (position < text.size() && (text[position].isDigit() || text[position] == '.'))
            ++position;
        if (position < text.size() && text[position] == 'e') {
            ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-'))
                ++position;
            while (position < text.size() && text[position].isDigit())
                ++position;
        }
        bool number_valid = false;
        const auto value = text.mid(start, position - start).toDouble(&number_valid);
        valid &= number_valid;
        return value;
    }

public:
    explicit Calculator(QString text) : text(std::move(text)) {}
    std::optional<double> run() {
        const auto value = expression();
        spaces();
        if (!valid || position != text.size() || !std::isfinite(value))
            return {};
        return value == 0 ? 0 : value;
    }
};
}

search_query::calculation search_query::calculate(const QString &text) {
    auto value = text.trimmed().toLower();
    if (value.isEmpty() || value.size() > 1024)
        return {};
    value.replace(',', '.').replace(QChar(0x00d7), '*').replace(QChar(0x00f7), '/').replace("**", "^");
    auto remaining = value;
    remaining.remove("sqrt");
    static const QRegularExpression permitted(QStringLiteral("^[0-9e.\\s+*/%()^√-]+$"));
    static const QRegularExpression digit("[0-9]");
    if (!permitted.match(remaining).hasMatch() || !digit.match(remaining).hasMatch())
        return {};
    return {true, Calculator(value).run()};
}
