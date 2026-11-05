#include <memory>
#include <QtTest>

#include "UiRegistry.h"

class UiRegistryTest : public QObject {
    Q_OBJECT
private slots:
    void configuration_is_atomic_data() {
        QTest::addColumn<int>("failure");
        QTest::newRow("success") << 0;
        QTest::newRow("standard exception") << 1;
        QTest::newRow("unknown exception") << 2;
    }

    void configuration_is_atomic() {
        QFETCH(int, failure);
        UiRegistry registry;
        registry.add_default("region", [](const ui_context &, QWidget *) {
            auto *widget = new QWidget;
            widget->setProperty("source", "builtin");
            return widget;
        });
        registry.append_style("builtin style");
        const auto previous_style = registry.style_sheet();
        const auto error = registry.configure_transaction([failure](UiRegistry &candidate) {
            candidate.append_style("plugin style");
            candidate.add_default("plugin region", [](const ui_context &, QWidget *) { return new QWidget; });
            candidate.add_default("region", [](const ui_context &, QWidget *) {
                auto *widget = new QWidget;
                widget->setProperty("source", "plugin default");
                return widget;
            });
            candidate.replace("region", [](const ui_context &, QWidget *) {
                auto *widget = new QWidget;
                widget->setProperty("source", "plugin");
                return widget;
            });
            if (failure == 1)
                throw std::runtime_error("configuration failed");
            if (failure == 2)
                throw 42;
        });
        QCOMPARE(error.isEmpty(), failure == 0);
        const std::unique_ptr<QWidget> widget(registry.create("region", {}, nullptr));
        QCOMPARE(widget->property("source").toString(), failure ? QString("builtin") : QString("plugin"));
        const std::unique_ptr<QWidget> default_widget(registry.create_default("region", {}, nullptr));
        QCOMPARE(default_widget->property("source").toString(),
                 failure ? QString("builtin") : QString("plugin default"));
        QCOMPARE(registry.regions().contains("plugin region"), failure == 0);
        if (failure)
            QCOMPARE(registry.style_sheet(), previous_style);
        else
            QVERIFY(registry.style_sheet().contains("plugin style"));
        // A failed module must not prevent a subsequent module from publishing.
        QVERIFY(registry.configure_transaction([](UiRegistry &candidate) { candidate.append_style("next module"); })
                    .isEmpty());
        QVERIFY(registry.style_sheet().contains("next module"));
    }

    void configuration_preserves_registry_address() {
        UiRegistry registry;
        QVERIFY(registry
                    .configure_transaction([](UiRegistry &candidate) {
                        candidate.replace("captured registry", [&candidate](const ui_context &context, QWidget *) {
                            auto *widget = new QWidget;
                            widget->setProperty("uses_live_registry", &candidate == context.registry);
                            return widget;
                        });
                    })
                    .isEmpty());
        ui_context context;
        context.registry = &registry;
        {
            const std::unique_ptr<QWidget> widget(registry.create("captured registry", context, nullptr));
            QVERIFY(widget->property("uses_live_registry").toBool());
        }
        QVERIFY(!registry
                     .configure_transaction([](UiRegistry &candidate) {
                         candidate.replace("captured registry",
                                           [](const ui_context &, QWidget *) { return new QWidget; });
                         throw std::runtime_error("rollback replacement");
                     })
                     .isEmpty());
        const std::unique_ptr<QWidget> widget(registry.create("captured registry", context, nullptr));
        QVERIFY(widget->property("uses_live_registry").toBool());
    }

    void compatibility_data() {
        QTest::addColumn<QJsonObject>("metadata");
        QTest::addColumn<bool>("compatible");
        const QJsonObject current{{"IID", XLIBRARY_UI_PLUGIN_IID}, {"version", QT_VERSION}};
        QTest::newRow("IID 1.0 without custom metadata") << current << true;
        auto metadata = current;
        metadata["IID"] = "org.xlibrary.desktop.UiPlugin/2.0";
        QTest::newRow("different interface") << metadata << false;
        metadata = current;
        metadata["version"] = QT_VERSION_CHECK(QT_VERSION_MAJOR + 1, 0, 0);
        QTest::newRow("different Qt major") << metadata << false;
        metadata["version"] = QT_VERSION_CHECK(QT_VERSION_MAJOR, QT_VERSION_MINOR + 1, 0);
        QTest::newRow("newer Qt minor") << metadata << false;
        metadata = current;
        metadata["version"] = QT_VERSION_CHECK(QT_VERSION_MAJOR, 0, 0);
        QTest::newRow("older Qt minor") << metadata << true;
        metadata.remove("version");
        QTest::newRow("missing Qt version") << metadata << false;
        metadata["version"] = "6.8.2";
        QTest::newRow("malformed Qt version") << metadata << false;
        metadata["version"] = 395264.5;
        QTest::newRow("fractional Qt version") << metadata << false;
        metadata["version"] = -1;
        QTest::newRow("negative Qt version") << metadata << false;
        metadata["version"] = 0x1000000;
        QTest::newRow("out of range Qt version") << metadata << false;
        QTest::newRow("not a Qt plugin") << QJsonObject{} << false;
    }

    void compatibility() {
        QFETCH(QJsonObject, metadata);
        QFETCH(bool, compatible);
        QCOMPARE(UiRegistry::plugin_compatibility_error(metadata).isEmpty(), compatible);
    }
};

QTEST_MAIN(UiRegistryTest)
#include "ui_registry_test.moc"
