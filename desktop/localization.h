#ifndef FORADECK_LOCALIZATION_H
#define FORADECK_LOCALIZATION_H

#include <QCoreApplication>
#include <QTranslator>

class Localization {
    class ShortcutTranslator : public QTranslator {
    public:
        bool isEmpty() const override { return false; }
        QString translate(const char *context, const char *source_text, const char * = nullptr,
                          int = -1) const override;
    };

    ShortcutTranslator shortcut_names;
    QTranslator russian;
    QTranslator qt_russian;

public:
    void apply(const QString &preference = "system");
};

#endif
