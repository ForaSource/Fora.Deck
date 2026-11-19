#include <QProgressBar>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QtTest>

#include "NotificationBanner.h"

namespace desktop_platform {
bool animations_enabled() {
    return false;
}
}

class NotificationBannerTest : public QObject {
    Q_OBJECT
private slots:
    void dismiss_and_replace_message() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        NotificationBanner banner(settings);
        QVERIFY(!banner.isVisible());
        banner.show_message("Scan finished", "One entry added", "Show details");
        QVERIFY(banner.isVisible());
        const auto progress = banner.findChild<QProgressBar *>("notification_progress");
        QVERIFY(progress);
        QCOMPARE(progress->maximum(), 7000);
        const auto button = banner.findChild<QToolButton *>("notification_dismiss");
        QVERIFY(button);
        QVERIFY(!button->accessibleName().isEmpty());

        QLabel *action_label = nullptr;
        for (auto *label : banner.findChildren<QLabel *>()) {
            if (label->text().contains("Show details"))
                action_label = label;
        }
        QVERIFY(action_label);
        QVERIFY(action_label->textInteractionFlags().testFlag(Qt::LinksAccessibleByKeyboard));
        QTextDocument message;
        message.setHtml(action_label->text());
        const auto link = message.find("Show details").charFormat();
        QVERIFY(link.isAnchor());
        QVERIFY(!link.anchorHref().isEmpty());
        int activations = 0;
        banner.activated = [&] { ++activations; };
        // Native focus traversal depends on the window manager. Exercise the label-to-banner connection directly.
        action_label->linkActivated(link.anchorHref());
        QCOMPARE(activations, 1);

        banner.show_message("Index ready", {}, {}, false);
        QCOMPARE(action_label->textFormat(), Qt::PlainText);
        QCOMPARE(action_label->text(), QString("Index ready"));
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(!banner.isVisible());
        QCOMPARE(activations, 1);
    }

    void countdown_waits_until_parent_is_visible() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        QWidget parent;
        NotificationBanner banner(settings, 0, 40, &parent);
        banner.show_message("Short notice");
        // Give an incorrectly running countdown time to expire while the window is hidden.
        QTest::qWait(120);
        parent.show();
        QVERIFY(banner.isVisible());
        QTRY_VERIFY(!banner.isVisible());
    }
};

QTEST_MAIN(NotificationBannerTest)
#include "notification_banner_test.moc"
