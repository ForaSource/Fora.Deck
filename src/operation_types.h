#ifndef XLIBRARY_OPERATION_TYPES_H
#define XLIBRARY_OPERATION_TYPES_H

#include <exception>
#include <QCoreApplication>
#include <QMetaType>
#include <QString>

#include "catalog_types.h"

enum class operation_state { loading, ready, error, cancelled, completed };

enum class operation_error_source { none, validation, sqlite, filesystem, platform, library, internal, updates };

inline QString operation_source_name(operation_error_source source) {
    switch (source) {
    case operation_error_source::none:
        return "none";
    case operation_error_source::validation:
        return "validation";
    case operation_error_source::sqlite:
        return "SQLite";
    case operation_error_source::filesystem:
        return "filesystem";
    case operation_error_source::platform:
        return "platform";
    case operation_error_source::library:
        return "library";
    case operation_error_source::internal:
        return "internal";
    case operation_error_source::updates:
        return "updates";
    }
    return "unknown";
}

struct operation_error {
    QString user_message;
    QString technical_reason;
    operation_error_source source = operation_error_source::none;
};

struct operation_result {
    quint64 id = 0;
    QString operation;
    operation_state state = operation_state::ready;
    operation_error error;
};

Q_DECLARE_METATYPE(operation_state)
Q_DECLARE_METATYPE(operation_error_source)
Q_DECLARE_METATYPE(operation_error)
Q_DECLARE_METATYPE(operation_result)

inline operation_error exception_error(const QString &operation, const std::exception &error,
                                       operation_error_source source) {
    const auto reason = QString::fromUtf8(error.what());
    QString message;
    if (source == operation_error_source::validation || source == operation_error_source::platform)
        message = reason;
    else if (operation == "library")
        message = QCoreApplication::translate("XLibrary", "Could not open the library: %1").arg(reason);
    else if (operation == "application")
        message = QCoreApplication::translate("XLibrary", "Could not save the application.");
    else if (operation == "tag")
        message = QCoreApplication::translate("XLibrary", "Could not save the tag.");
    else if (operation == "scenario")
        message = QCoreApplication::translate("XLibrary", "Could not save the scenario.");
    else
        message = QCoreApplication::translate("XLibrary", "The operation could not be completed: %1").arg(reason);
    return {message, reason, source};
}

inline QString discovery_summary(qsizetype added, const QVector<discovery_issue> &issues, const QString &status) {
    qsizetype duplicates = 0;
    qsizetype skipped = 0;
    qsizetype errors = 0;
    for (const auto &issue : issues) {
        switch (issue.kind) {
        case discovery_issue_kind::duplicate:
            ++duplicates;
            break;
        case discovery_issue_kind::skipped:
            ++skipped;
            break;
        case discovery_issue_kind::error:
            ++errors;
            break;
        }
    }
    return QCoreApplication::translate("XLibrary", "%1 Added: %2. Duplicates: %3. Skipped: %4. Errors: %5.")
        .arg(status)
        .arg(added)
        .arg(duplicates)
        .arg(skipped)
        .arg(errors);
}

inline QString discovery_details(const QString &summary, const QVector<discovery_issue> &issues) {
    QStringList details{summary};
    for (const auto &issue : issues) {
        const auto kind =
            issue.kind == discovery_issue_kind::duplicate ? QCoreApplication::translate("XLibrary", "Duplicate")
            : issue.kind == discovery_issue_kind::skipped ? QCoreApplication::translate("XLibrary", "Skipped")
                                                          : QCoreApplication::translate("XLibrary", "Error");
        details.append(QString("[%1] %2: %3").arg(kind, issue.source, issue.reason));
    }
    return details.join('\n');
}

#endif
