#ifndef XLIBRARY_UI_REGISTRY_H
#define XLIBRARY_UI_REGISTRY_H

#include <functional>
#include <QAbstractItemModel>
#include <QAction>
#include <QCoreApplication>
#include <QHash>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSettings>
#include <QWidget>
#include <stdexcept>

#define XLIBRARY_UI_PLUGIN_IID "org.xlibrary.desktop.UiPlugin/1.0"

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
    // Qt supplies IID and version without executing the module. IID 1.0 modules
    // need no custom metadata; their compiler/runtime ABI remains a trust decision.
    static QString plugin_compatibility_error(const QJsonObject &metadata) {
        if (metadata.value("IID").toString() != XLIBRARY_UI_PLUGIN_IID)
            return QCoreApplication::translate("XLibrary", "Unsupported UI plugin interface (requires IID 1.0).");
        const auto version = metadata.value("version").toInt(-1);
        if (version < 0 || version > 0xffffff)
            return QCoreApplication::translate("XLibrary", "Missing or invalid Qt version metadata.");
        if ((version >> 16) != QT_VERSION_MAJOR)
            return QCoreApplication::translate("XLibrary", "Incompatible Qt major version.");
        if (((version >> 8) & 0xff) > QT_VERSION_MINOR)
            return QCoreApplication::translate("XLibrary", "Plugin requires a newer Qt minor version.");
        return {};
    }

    template <typename Configure> QString configure_transaction(Configure &&configure) {
        try {
            auto original = *this;
            auto rollback = qScopeGuard([this, &original] {
                // Restore without allocation before an exception reaches the caller.
                defaults.swap(original.defaults);
                overrides.swap(original.overrides);
                additional_style.swap(original.additional_style);
            });
            // Existing IID 1.0 factories may retain a reference to this registry.
            configure(*this);
            rollback.dismiss();
            return {};
        } catch (const std::exception &error) {
            return QCoreApplication::translate("XLibrary", "Plugin configuration failed: %1")
                .arg(QString::fromUtf8(error.what()));
        } catch (...) {
            return QCoreApplication::translate("XLibrary",
                                               "Plugin configuration failed with an unknown C++ exception.");
        }
    }

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
    // The registry retains its address; failed configuration restores its contents.
    virtual void configure(UiRegistry &registry) = 0;
};

Q_DECLARE_INTERFACE(UiPlugin, XLIBRARY_UI_PLUGIN_IID)

#endif
