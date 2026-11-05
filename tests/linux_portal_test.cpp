#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

#include "PlatformIntegration.h"

struct TestShortcut {
    QString id;
    QVariantMap properties;
};
using TestShortcuts = QList<TestShortcut>;
Q_DECLARE_METATYPE(TestShortcut)
Q_DECLARE_METATYPE(TestShortcuts)
QDBusArgument &operator<<(QDBusArgument &argument, const TestShortcut &shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, TestShortcut &shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

// A private dbus-run-session tests the real request protocol; this is not a compositor mock.
class TestPortal : public QDBusVirtualObject {
public:
    uint version = 1;
    uint create_response = 0;
    uint bind_response = 0;
    bool empty_bindings = false;
    bool defer_response = false;
    QStringList sessions;
    QStringList requests;
    QStringList closed;
    TestShortcuts shortcuts;
    QString bound_session;
    int bind_count = 0;
    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        if (message.member() == "Get") {
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(version))));
            return true;
        }
        if (message.member() == "Close") {
            closed.append(message.path());
            connection.send(message.createReply());
            return true;
        }
        if (message.member() != "CreateSession" && message.member() != "BindShortcuts")
            return false;
        const auto options = qdbus_cast<QVariantMap>(message.arguments().last());
        auto sender = message.service().mid(1);
        sender.replace('.', '_');
        const auto request_path = "/org/freedesktop/portal/desktop/request/" + sender + '/' +
                                  options.value("handle_token").toString();
        requests.append(request_path);
        QVariantMap results;
        uint code = create_response;
        if (message.member() == "CreateSession") {
            const auto session_path = "/org/freedesktop/portal/desktop/session/" + sender + '/' +
                                      options.value("session_handle_token").toString();
            sessions.append(session_path);
            results.insert("session_handle", session_path);
        } else {
            ++bind_count;
            bound_session = qdbus_cast<QDBusObjectPath>(message.arguments().at(0)).path();
            shortcuts = qdbus_cast<TestShortcuts>(message.arguments().at(1));
            results.insert("shortcuts", QVariant::fromValue(empty_bindings ? TestShortcuts{} : shortcuts));
            code = bind_response;
        }
        connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request_path))));
        if (!defer_response) {
            QTimer::singleShot(0, this, [connection, request_path, results, code] {
                auto response = QDBusMessage::createSignal(request_path, "org.freedesktop.portal.Request", "Response");
                response.setArguments({code, results});
                connection.send(response);
            });
        }
        return true;
    }
    void activate(const QString &session, const QString &id) {
        auto signal = QDBusMessage::createSignal("/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.GlobalShortcuts", "Activated");
        signal.setArguments({QVariant::fromValue(QDBusObjectPath(session)), id,
                             QVariant::fromValue<qulonglong>(123), QVariantMap{}});
        QDBusConnection::sessionBus().send(signal);
    }
};

class LinuxPortalTest : public QObject {
    Q_OBJECT
    QDBusConnection bus = QDBusConnection::sessionBus();
    std::unique_ptr<TestPortal> portal;
private slots:
    void initTestCase() {
        QVERIFY(bus.isConnected());
        qputenv("XDG_SESSION_TYPE", "wayland");
        qDBusRegisterMetaType<TestShortcut>();
        qDBusRegisterMetaType<TestShortcuts>();
    }
    void init() {
        portal = std::make_unique<TestPortal>();
        QVERIFY(bus.registerVirtualObject("/org/freedesktop/portal/desktop", portal.get(), QDBusConnection::SubPath));
        QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
    }
    void cleanup() {
        bus.unregisterService("org.freedesktop.portal.Desktop");
        bus.unregisterObject("/org/freedesktop/portal/desktop", QDBusConnection::UnregisterTree);
        portal.reset();
    }
    void binds_activates_and_clears() {
        GlobalHotkey hotkey;
        QSignalSpy failures(&hotkey, &GlobalHotkey::registration_failed);
        QSignalSpy activations(&hotkey, &GlobalHotkey::activated);
        QString error;
        QVERIFY(hotkey.set_sequence(QKeySequence("Ctrl+Alt+Space"), error));
        QTRY_COMPARE(portal->bind_count, 1);
        QCOMPARE(portal->shortcuts.size(), 1);
        QCOMPARE(portal->shortcuts.front().properties.value("preferred_trigger").toString(), "CTRL+ALT+space");
        const auto session = portal->bound_session;
        const auto id = portal->shortcuts.front().id;
        // Ordered delivery on the same bus makes this activation follow the Bind Response.
        portal->activate(session, "unrelated_action");
        portal->activate("/unrelated_session", id);
        portal->activate(session, id);
        QTRY_COMPARE(activations.size(), 1);
        QCOMPARE(failures.size(), 0);
        QVERIFY(hotkey.set_sequence({}, error));
        QTRY_VERIFY(portal->closed.contains(session));
        portal->activate(session, id);
        QTest::qWait(20);
        QCOMPARE(activations.size(), 1);
    }
    void failures_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("unsupported-version") << 0;
        QTest::newRow("create-denied") << 1;
        QTest::newRow("bind-denied") << 2;
        QTest::newRow("no-bound-action") << 3;
    }
    void failures() {
        QFETCH(int, mode);
        portal->version = mode == 0 ? 0 : 1;
        portal->create_response = mode == 1 ? 1 : 0;
        portal->bind_response = mode == 2 ? 2 : 0;
        portal->empty_bindings = mode == 3;
        GlobalHotkey hotkey;
        QSignalSpy failures(&hotkey, &GlobalHotkey::registration_failed);
        QString error;
        QVERIFY(hotkey.set_sequence(QKeySequence("Ctrl+Alt+A"), error));
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(failures.front().front().toString().contains("XLibrary --overlay"));
        if (mode != 0)
            QTRY_VERIFY(portal->closed.contains(portal->sessions.front()));
        // Failure clears submission state, so saving the same binding can retry.
        QVERIFY(hotkey.set_sequence(QKeySequence("Ctrl+Alt+A"), error));
        QTRY_COMPARE(failures.size(), 2);
    }
    void unique_actions_and_replace() {
        GlobalHotkey first;
        GlobalHotkey second;
        QString error;
        QVERIFY(first.set_sequence(QKeySequence("Ctrl+Alt+A"), error));
        QTRY_COMPARE(portal->bind_count, 1);
        const auto first_id = portal->shortcuts.front().id;
        const auto first_session = portal->bound_session;
        QVERIFY(second.set_sequence(QKeySequence("Ctrl+Alt+B"), error));
        QTRY_COMPARE(portal->bind_count, 2);
        QVERIFY(portal->shortcuts.front().id != first_id);
        QVERIFY(first.set_sequence(QKeySequence("Ctrl+Alt+C"), error));
        QTRY_COMPARE(portal->bind_count, 3);
        QTRY_VERIFY(portal->closed.contains(first_session));
        QCOMPARE(portal->shortcuts.front().id, first_id);
    }
    void pending_request_is_closed_on_destruction() {
        portal->defer_response = true;
        auto hotkey = std::make_unique<GlobalHotkey>();
        QString error;
        QVERIFY(hotkey->set_sequence(QKeySequence("Ctrl+Alt+A"), error));
        QTRY_COMPARE(portal->requests.size(), 1);
        const auto request = portal->requests.front();
        const auto session = portal->sessions.front();
        hotkey.reset();
        QTRY_VERIFY(portal->closed.contains(request));
        QTRY_VERIFY(portal->closed.contains(session));
    }
    void session_loss_is_reported() {
        GlobalHotkey hotkey;
        QSignalSpy failures(&hotkey, &GlobalHotkey::registration_failed);
        QString error;
        QVERIFY(hotkey.set_sequence(QKeySequence("Ctrl+Alt+A"), error));
        QTRY_COMPARE(portal->bind_count, 1);
        auto signal = QDBusMessage::createSignal(portal->bound_session, "org.freedesktop.portal.Session", "Closed");
        signal.setArguments({QVariantMap{}});
        bus.send(signal);
        QTRY_COMPARE(failures.size(), 1);
    }
    void absent_service_is_reported() {
        QVERIFY(bus.unregisterService("org.freedesktop.portal.Desktop"));
        GlobalHotkey hotkey;
        QSignalSpy failures(&hotkey, &GlobalHotkey::registration_failed);
        QString error;
        QVERIFY(hotkey.set_sequence(QKeySequence("Ctrl+Alt+A"), error));
        QTRY_COMPARE(failures.size(), 1);
    }
};
QTEST_MAIN(LinuxPortalTest)
#include "linux_portal_test.moc"
