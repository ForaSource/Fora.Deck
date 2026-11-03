#include <QLocale>

#include "localization.h"

QString Localization::ShortcutTranslator::translate(const char *const context, const char *const source_text,
                                                    const char *, int) const {
    // Change only the Qt label; key codes and PortableText in settings remain unchanged.
    if (qstrcmp(context, "QShortcut") == 0 && qstrcmp(source_text, "Return") == 0)
        return QStringLiteral("Enter");
    return {};
}

void Localization::apply(const QString &preference) {
    QCoreApplication::removeTranslator(&shortcut_names);
    QCoreApplication::removeTranslator(&russian);
    QCoreApplication::removeTranslator(&qt_russian);
    QCoreApplication::installTranslator(&shortcut_names);
    QString language = preference;
    if (language == "system") {
        language = "en";
        for (const auto &name : QLocale::system().uiLanguages()) {
            const auto candidate = QLocale(name).language();
            if (candidate == QLocale::Russian || candidate == QLocale::English) {
                language = candidate == QLocale::Russian ? "ru" : "en";
                break;
            }
        }
    }
    // English source text remains available for unknown locales or translation loading failures.
    if (language != "ru" || !russian.load(":/i18n/xlibrary_ru.qm") || !qt_russian.load(":/i18n/qt_widgets_ru.qm"))
        return;
    QCoreApplication::installTranslator(&qt_russian);
    QCoreApplication::installTranslator(&russian);
}
