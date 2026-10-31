#include <cmath>
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>

#include "search_query.h"

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

namespace {
QString normalized_search(const QString &text) {
    return text.normalized(QString::NormalizationForm_C).toCaseFolded().simplified();
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
            terms.append({word, search_query::switch_layout(word)});
            names.append(word);
        }
    }
    phrase = names.join(' ');
    alternate_phrase = search_query::switch_layout(phrase);
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
    if (incomplete_tag)
        return -1;
    QStringList normalized_tags;
    for (const auto &tag : tags)
        normalized_tags.append(normalized_search(tag));
    for (const auto &tag : required_tags)
        if (!normalized_tags.contains(tag))
            return -1;
    for (const auto &tag : excluded_tags)
        if (normalized_tags.contains(tag))
            return -1;
    if (terms.isEmpty())
        return 0;
    QStringList fields{normalized_search(name), normalized_search(file_name)};
    for (const auto &alias : aliases)
        fields.append(normalized_search(alias));
    bool literal_match = true;
    for (const auto &term : terms) {
        bool literal = false;
        bool alternate = false;
        for (const auto &field : fields) {
            literal |= field.contains(term.literal);
            alternate |= field.contains(term.alternate);
        }
        if (!literal && !alternate)
            return -1;
        literal_match &= literal;
    }
    int best = literal_match ? 4 : 5;
    for (const auto &field : fields) {
        if (field == phrase)
            best = qMin(best, 0);
        else if (field == alternate_phrase)
            best = qMin(best, 1);
        else if (field.startsWith(phrase))
            best = qMin(best, 2);
        else if (field.startsWith(alternate_phrase))
            best = qMin(best, 3);
    }
    return best;
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
    bool take(QChar character) {
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
