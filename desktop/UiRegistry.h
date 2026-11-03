#ifndef XLIBRARY_UI_REGISTRY_H
#define XLIBRARY_UI_REGISTRY_H

#include <functional>
#include <QAbstractItemModel>
#include <QAction>
#include <QHash>
#include <QSettings>
#include <QWidget>
#include <stdexcept>

class UiRegistry;

struct ui_context {
    QObject *service = nullptr;
    QAbstractItemModel *catalog = nullptr;
    QAbstractItemModel *filtered_catalog = nullptr;
    QSettings *settings = nullptr;
    QHash<QString, QAction *> actions;
    UiRegistry *registry = nullptr;
    QWidget *window = nullptr;
    QVariantMap parameters;
};

class UiRegistry {
public:
    using widget_factory = std::function<QWidget *(const ui_context &, QWidget *)>;

private:
    QHash<QString, widget_factory> defaults;
    QHash<QString, widget_factory> overrides;
    QString additional_style;

public:
    void add_default(const QString &id, widget_factory factory) { defaults.insert(id, std::move(factory)); }
    void replace(const QString &id, widget_factory factory) { overrides.insert(id, std::move(factory)); }
    void append_style(const QString &style) { additional_style += '\n' + style; }
    QString style_sheet() const { return additional_style; }
    QStringList regions() const { return defaults.keys(); }
    QWidget *create_default(const QString &id, const ui_context &context, QWidget *const parent) const {
        const auto factory = defaults.constFind(id);
        if (factory == defaults.cend())
            throw std::runtime_error("Unknown UI region: " + id.toStdString());
        auto *result = (*factory)(context, parent);
        if (!result)
            throw std::runtime_error("UI factory returned no widget: " + id.toStdString());
        result->setObjectName(id);
        return result;
    }
    QWidget *create(const QString &id, const ui_context &context, QWidget *const parent) const {
        QWidget *result = nullptr;
        const auto custom = overrides.constFind(id);
        if (custom != overrides.cend())
            result = (*custom)(context, parent);
        if (!result) {
            result = create_default(id, context, parent);
        }
        if (!result)
            throw std::runtime_error("UI factory returned no widget: " + id.toStdString());
        result->setObjectName(id);
        return result;
    }
};

class UiPlugin {
public:
    virtual ~UiPlugin() = default;
    virtual void configure(UiRegistry &registry) = 0;
};

#define XLIBRARY_UI_PLUGIN_IID "org.xlibrary.desktop.UiPlugin/1.0"
Q_DECLARE_INTERFACE(UiPlugin, XLIBRARY_UI_PLUGIN_IID)

#endif
