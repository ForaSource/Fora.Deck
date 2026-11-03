#include <limits>
#include <QColor>
#include <QDataStream>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSettings>
#include <QSize>
#include <QTimeZone>
#include <QUrl>
#include <stdexcept>

#include "profile_settings.h"

namespace {
constexpr qsizetype max_value_bytes = 16 * 1024 * 1024;
constexpr int max_depth = 32;
constexpr qsizetype max_items = 100000;

[[noreturn]] void invalid_setting() {
    throw std::runtime_error("Unsupported or malformed profile setting");
}

template <typename T> QByteArray stream_value(const T &value) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << value;
    if (stream.status() != QDataStream::Ok)
        invalid_setting();
    return bytes;
}

template <typename T> T fixed_value(const QByteArray &bytes, qsizetype expected_size) {
    if (bytes.size() != expected_size)
        invalid_setting();
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_5);
    T result{};
    stream >> result;
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        invalid_setting();
    return result;
}

QByteArray base64_value(const QJsonValue &value) {
    if (!value.isString())
        invalid_setting();
    const auto result =
        QByteArray::fromBase64Encoding(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        invalid_setting();
    return result.decoded;
}

QJsonObject pack(const QVariant &value, int depth, qsizetype &items) {
    if (depth > max_depth || ++items > max_items)
        invalid_setting();
    QString type;
    QJsonValue encoded;
    switch (value.metaType().id()) {
    case QMetaType::UnknownType:
        type = "invalid";
        encoded = QJsonValue::Null;
        break;
    case QMetaType::Bool:
        type = "bool";
        encoded = value.toBool();
        break;
    case QMetaType::Int:
        type = "int";
        encoded = value.toString();
        break;
    case QMetaType::UInt:
        type = "uint";
        encoded = value.toString();
        break;
    case QMetaType::LongLong:
        type = "qlonglong";
        encoded = value.toString();
        break;
    case QMetaType::ULongLong:
        type = "qulonglong";
        encoded = value.toString();
        break;
    case QMetaType::Double:
        type = "double";
        encoded = QString::fromLatin1(stream_value(value.toDouble()).toBase64());
        break;
    case QMetaType::Float:
        type = "float";
        encoded = QString::fromLatin1(stream_value(value.toFloat()).toBase64());
        break;
    case QMetaType::QString:
        type = "QString";
        encoded = value.toString();
        break;
    case QMetaType::QByteArray:
        type = "QByteArray";
        encoded = QString::fromLatin1(value.toByteArray().toBase64());
        break;
    case QMetaType::QStringList: {
        type = "QStringList";
        QJsonArray list;
        for (const auto &entry : value.toStringList()) {
            if (++items > max_items)
                invalid_setting();
            list.append(entry);
        }
        encoded = list;
        break;
    }
    case QMetaType::QVariantList: {
        type = "QVariantList";
        QJsonArray list;
        for (const auto &entry : value.toList())
            list.append(pack(entry, depth + 1, items));
        encoded = list;
        break;
    }
    case QMetaType::QVariantMap:
    case QMetaType::QVariantHash: {
        type = value.metaType().id() == QMetaType::QVariantMap ? "QVariantMap" : "QVariantHash";
        QJsonObject map;
        const auto entries = value.toMap();
        for (auto it = entries.cbegin(); it != entries.cend(); ++it)
            map.insert(it.key(), pack(it.value(), depth + 1, items));
        encoded = map;
        break;
    }
    case QMetaType::QDate:
        type = "QDate";
        encoded = QString::number(value.toDate().toJulianDay());
        break;
    case QMetaType::QTime:
        type = "QTime";
        encoded = QString::number(value.toTime().msecsSinceStartOfDay());
        break;
    case QMetaType::QDateTime: {
        type = "QDateTime";
        const auto date_time = value.toDateTime();
        QJsonObject object;
        object.insert("valid", date_time.isValid());
        object.insert("time", QString::number(date_time.toMSecsSinceEpoch()));
        object.insert("zone", QString::fromLatin1(date_time.timeZone().id().toBase64()));
        object.insert("spec", static_cast<int>(date_time.timeSpec()));
        object.insert("offset", date_time.offsetFromUtc());
        encoded = object;
        break;
    }
    case QMetaType::QUrl:
        type = "QUrl";
        encoded = QString::fromLatin1(value.toUrl().toEncoded().toBase64());
        break;
    case QMetaType::QPoint:
        type = "QPoint";
        encoded = QString::fromLatin1(stream_value(value.toPoint()).toBase64());
        break;
    case QMetaType::QPointF:
        type = "QPointF";
        encoded = QString::fromLatin1(stream_value(value.toPointF()).toBase64());
        break;
    case QMetaType::QSize:
        type = "QSize";
        encoded = QString::fromLatin1(stream_value(value.toSize()).toBase64());
        break;
    case QMetaType::QSizeF:
        type = "QSizeF";
        encoded = QString::fromLatin1(stream_value(value.toSizeF()).toBase64());
        break;
    case QMetaType::QRect:
        type = "QRect";
        encoded = QString::fromLatin1(stream_value(value.toRect()).toBase64());
        break;
    case QMetaType::QRectF:
        type = "QRectF";
        encoded = QString::fromLatin1(stream_value(value.toRectF()).toBase64());
        break;
    case QMetaType::QColor:
        type = "QColor";
        encoded = value.value<QColor>().name(QColor::HexArgb);
        break;
    case QMetaType::QFont:
        type = "QFont";
        encoded = value.value<QFont>().toString();
        break;
    case QMetaType::QKeySequence:
        type = "QKeySequence";
        encoded = value.value<QKeySequence>().toString(QKeySequence::PortableText);
        break;
    default:
        invalid_setting();
    }
    return {{"type", type}, {"value", encoded}};
}

qint64 signed_number(const QJsonValue &value) {
    if (!value.isString())
        invalid_setting();
    bool ok = false;
    const auto result = value.toString().toLongLong(&ok);
    if (!ok)
        invalid_setting();
    return result;
}

quint64 unsigned_number(const QJsonValue &value) {
    if (!value.isString() || value.toString().startsWith('-'))
        invalid_setting();
    bool ok = false;
    const auto result = value.toString().toULongLong(&ok);
    if (!ok)
        invalid_setting();
    return result;
}

QVariant unpack(const QJsonObject &object, int depth, qsizetype &items) {
    if (depth > max_depth || ++items > max_items || object.size() != 2 || !object.value("type").isString() ||
        !object.contains("value"))
        invalid_setting();
    const auto type = object.value("type").toString();
    const auto value = object.value("value");
    if (type == "invalid" && value.isNull())
        return {};
    if (type == "bool" && value.isBool())
        return value.toBool();
    if (type == "int") {
        const auto number = signed_number(value);
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
            invalid_setting();
        return static_cast<int>(number);
    }
    if (type == "uint") {
        const auto number = unsigned_number(value);
        if (number > std::numeric_limits<uint>::max())
            invalid_setting();
        return static_cast<uint>(number);
    }
    if (type == "qlonglong")
        return signed_number(value);
    if (type == "qulonglong")
        return unsigned_number(value);
    if (type == "double")
        return fixed_value<double>(base64_value(value), 8);
    if (type == "float")
        return fixed_value<float>(base64_value(value), 8);
    if (type == "QString" && value.isString())
        return value.toString();
    if (type == "QByteArray")
        return base64_value(value);
    if (type == "QStringList" && value.isArray()) {
        QStringList list;
        for (const auto &entry : value.toArray()) {
            if (++items > max_items || !entry.isString())
                invalid_setting();
            list.append(entry.toString());
        }
        return list;
    }
    if (type == "QVariantList" && value.isArray()) {
        QVariantList list;
        for (const auto &entry : value.toArray()) {
            if (!entry.isObject())
                invalid_setting();
            list.append(unpack(entry.toObject(), depth + 1, items));
        }
        return list;
    }
    if ((type == "QVariantMap" || type == "QVariantHash") && value.isObject()) {
        QVariantMap map;
        const auto entries = value.toObject();
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
            if (!it.value().isObject())
                invalid_setting();
            map.insert(it.key(), unpack(it.value().toObject(), depth + 1, items));
        }
        if (type == "QVariantHash") {
            QVariantHash hash;
            for (auto it = map.cbegin(); it != map.cend(); ++it)
                hash.insert(it.key(), it.value());
            return hash;
        }
        return map;
    }
    if (type == "QDate")
        return QDate::fromJulianDay(signed_number(value));
    if (type == "QTime") {
        const auto time = signed_number(value);
        if (time < -1 || time >= 86400000)
            invalid_setting();
        return QTime::fromMSecsSinceStartOfDay(static_cast<int>(time));
    }
    if (type == "QDateTime" && value.isObject()) {
        const auto fields = value.toObject();
        if (!fields.value("valid").isBool())
            invalid_setting();
        if (!fields.value("valid").toBool())
            return QDateTime{};
        const auto time = signed_number(fields.value("time"));
        const auto spec = fields.value("spec").toInt(-1);
        const auto offset = fields.value("offset").toInt(1000000);
        QTimeZone zone;
        switch (spec) {
        case Qt::LocalTime:
            zone = QTimeZone::systemTimeZone();
            break;
        case Qt::UTC:
            zone = QTimeZone::utc();
            break;
        case Qt::OffsetFromUTC:
            if (offset < -50400 || offset > 50400)
                invalid_setting();
            zone = QTimeZone(offset);
            break;
        case Qt::TimeZone:
            zone = QTimeZone(base64_value(fields.value("zone")));
            break;
        default:
            invalid_setting();
        }
        if (!zone.isValid())
            invalid_setting();
        if (spec == Qt::LocalTime)
            return QDateTime::fromMSecsSinceEpoch(time, QTimeZone::utc()).toLocalTime();
        return QDateTime::fromMSecsSinceEpoch(time, zone);
    }
    if (type == "QUrl")
        return QUrl::fromEncoded(base64_value(value), QUrl::StrictMode);
    if (type == "QPoint")
        return fixed_value<QPoint>(base64_value(value), 8);
    if (type == "QPointF")
        return fixed_value<QPointF>(base64_value(value), 16);
    if (type == "QSize")
        return fixed_value<QSize>(base64_value(value), 8);
    if (type == "QSizeF")
        return fixed_value<QSizeF>(base64_value(value), 16);
    if (type == "QRect")
        return fixed_value<QRect>(base64_value(value), 16);
    if (type == "QRectF")
        return fixed_value<QRectF>(base64_value(value), 32);
    if (type == "QColor" && value.isString()) {
        const QColor color(value.toString());
        if (!color.isValid())
            invalid_setting();
        return color;
    }
    if (type == "QFont" && value.isString()) {
        QFont font;
        if (!font.fromString(value.toString()))
            invalid_setting();
        return font;
    }
    if (type == "QKeySequence" && value.isString())
        return QVariant::fromValue(QKeySequence::fromString(value.toString(), QKeySequence::PortableText));
    invalid_setting();
}
}

profile_settings::values profile_settings::read(const QString &path) {
    if (QFileInfo::exists(path) && !QFileInfo(path).isFile())
        throw std::runtime_error("Profile settings are not a regular file");
    QSettings settings(path, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    values result;
    for (const auto &key : settings.allKeys()) {
        if (key.compare(QLatin1String(token_marker), Qt::CaseInsensitive) != 0)
            result.insert(key, settings.value(key));
    }
    if (settings.status() != QSettings::NoError)
        throw std::runtime_error("Cannot read profile settings");
    validate_keys(result);
    return result;
}

void profile_settings::write(const QString &path, const values &settings) {
    QSettings output(path, QSettings::IniFormat);
    output.setFallbacksEnabled(false);
    output.clear();
    for (auto it = settings.cbegin(); it != settings.cend(); ++it)
        output.setValue(it.key(), it.value());
    output.sync();
    if (output.status() != QSettings::NoError)
        throw std::runtime_error("Cannot write profile settings");
}

QByteArray profile_settings::encode(const QVariant &value) {
    qsizetype items = 0;
    auto bytes = QJsonDocument(pack(value, 0, items)).toJson(QJsonDocument::Compact);
    if (bytes.size() > max_value_bytes)
        invalid_setting();
    return bytes;
}

QVariant profile_settings::decode(const QByteArray &json) {
    if (json.size() > max_value_bytes)
        invalid_setting();
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        invalid_setting();
    qsizetype items = 0;
    return unpack(document.object(), 0, items);
}

void profile_settings::validate_keys(const values &settings) {
    QSet<QString> portable_keys;
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        const auto &key = it.key();
        const auto folded = key.toCaseFolded();
        // QSettings нормализует разделители; Windows также не различает регистр INI-ключей.
        // Отказ предотвращает тихое слияние настроек и обход локального маркера токена.
        if (key.isEmpty() || key.size() > 32768 || key.contains(QChar::Null) || key.startsWith('/') ||
            key.endsWith('/') || key.contains("//") || key.contains('\\') || portable_keys.contains(folded) ||
            folded == QLatin1String(token_marker))
            invalid_setting();
        portable_keys.insert(folded);
    }
}
