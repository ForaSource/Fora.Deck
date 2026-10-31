#include <cmath>
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

SearchQuery::SearchQuery(const QString &text) {
    const auto words = text.toCaseFolded().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList names;
    for (const auto &word : words) {
        if (word.startsWith("-#"))
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

int SearchQuery::rank(const QString &name, const QString &file_name, const QStringList &tags) const {
    for (const auto &tag : required_tags)
        if (!tags.contains(tag, Qt::CaseInsensitive))
            return -1;
    for (const auto &tag : excluded_tags)
        if (tags.contains(tag, Qt::CaseInsensitive))
            return -1;
    if (terms.isEmpty())
        return 0;
    bool literal_match = true;
    for (const auto &term : terms) {
        const bool literal = name.contains(term.literal) || file_name.contains(term.literal);
        if (!literal && !name.contains(term.alternate) && !file_name.contains(term.alternate))
            return -1;
        literal_match &= literal;
    }
    if (name == phrase)
        return 0;
    if (name == alternate_phrase)
        return 1;
    if (file_name == phrase)
        return 2;
    if (file_name == alternate_phrase)
        return 3;
    if (name.startsWith(phrase))
        return 4;
    if (name.startsWith(alternate_phrase))
        return 5;
    return literal_match ? 6 : 7;
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
