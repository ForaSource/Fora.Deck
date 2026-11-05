#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include "UpdateChecker.h"

// The offline checker test never invokes OS installation. Keep platform side effects outside this target.
namespace desktop_platform {
bool installed_for_updates() {
    return false;
}
bool start_update_installer(const QString &, QString &) {
    return false;
}
}

namespace {
QJsonObject release(const QString &tag, bool preview = false) {
    return {{"tag_name", tag},
            {"draft", false},
            {"prerelease", preview},
            {"published_at", "2026-11-05T12:00:00Z"},
            {"html_url", "https://github.com/lappland-open/XLibrary/releases/tag/" + tag}};
}
QByteArray json(const QJsonArray &releases) {
    return QJsonDocument(releases).toJson();
}
}

class UpdateCheckerTest : public QObject {
    Q_OBJECT
private slots:
    void channels() {
        const auto releases =
            json({release("0.7.0"), release("0.8.0-rc.1", true), release("0.8.0-beta.2", true), release("0.6.9")});
        const auto stable = UpdateChecker::select_release_page(releases, "0.6.0", "stable");
        QVERIFY(stable.error.isEmpty());
        QCOMPARE(stable.release.value("tag_name").toString(), "0.7.0");
        QVERIFY(stable.update_available);
        const auto preview = UpdateChecker::select_release_page(releases, "0.6.0", "preview");
        QCOMPARE(preview.release.value("tag_name").toString(), "0.8.0-rc.1");
        // GitHub's field, rather than a guessed suffix, defines the channel.
        QCOMPARE(UpdateChecker::select_release_page(json({release("0.9.0", true)}), "0.6.0", "stable").release,
                 QJsonObject{});
        QCOMPARE(UpdateChecker::select_release_page(json({release("0.9.0-rc.1")}), "0.6.0", "stable")
                     .release.value("tag_name")
                     .toString(),
                 "0.9.0-rc.1");
    }
    void semver_data() {
        QTest::addColumn<QString>("older");
        QTest::addColumn<QString>("newer");
        QTest::newRow("rc-final") << "1.0.0-rc.1" << "1.0.0";
        QTest::newRow("numeric") << "1.0.0-beta.2" << "1.0.0-beta.11";
        QTest::newRow("numeric-alpha") << "1.0.0-1" << "1.0.0-alpha";
        QTest::newRow("prefix") << "1.0.0-alpha" << "1.0.0-alpha.1";
        QTest::newRow("large") << "99999999999999999999.0.0" << "100000000000000000000.0.0";
        QTest::newRow("legacy") << "1.0.0-pre-release" << "1.0.0";
    }
    void semver() {
        QFETCH(QString, older);
        QFETCH(QString, newer);
        const auto order = UpdateChecker::compare_release_versions(older, newer);
        QVERIFY(order.has_value());
        QVERIFY(*order < 0);
        QVERIFY(*UpdateChecker::compare_release_versions(newer, older) > 0);
    }
    void no_downgrade() {
        for (const auto &current : {"0.7.0", "0.8.0", "0.9.0-rc.1"})
            QVERIFY(!UpdateChecker::select_release_page(json({release("0.7.0")}), current, "preview").update_available);
        QCOMPARE(*UpdateChecker::compare_release_versions("v1.0.0+one", "1.0.0+two"), 0);
        QVERIFY(!UpdateChecker::compare_release_versions("1.0.0-01", "1.0.0"));
        QVERIFY(!UpdateChecker::compare_release_versions("01.0.0", "1.0.0"));
    }
    void malformed_and_unpublished() {
        for (const auto &input : {QByteArray("{"), QByteArray("{}"), QByteArray("[false]"), QByteArray("[{}]")})
            QVERIFY(!UpdateChecker::select_release_page(input, "0.6.0", "preview").error.isEmpty());
        QVERIFY(!UpdateChecker::select_release_page("[]", "invalid", "stable").error.isEmpty());
        auto draft = release("9.0.0");
        draft["draft"] = true;
        auto unpublished = release("8.0.0");
        unpublished["published_at"] = "";
        QVERIFY(UpdateChecker::select_release_page(json({draft, unpublished, release("unknown")}), "0.6.0", "preview")
                    .release.isEmpty());
        auto unsafe = release("1.0.0");
        unsafe["html_url"] = "https://example.com";
        QVERIFY(!UpdateChecker::select_release_page(json({unsafe}), "0.6.0", "stable").error.isEmpty());
        QVERIFY(UpdateChecker::select_release_page("[]", "0.6.0", "stable").release.isEmpty());
        const auto partial =
            UpdateChecker::select_release_page(json({release("0.7.0"), QJsonObject{}}), "0.6.0", "stable");
        QVERIFY(!partial.error.isEmpty());
        QVERIFY(partial.release.isEmpty());
        QVERIFY(!partial.update_available);
    }
    void pagination_and_ties() {
        const auto first = release("0.7.0");
        const auto result =
            UpdateChecker::select_release_page(json({release("0.7.0"), release("0.6.9")}), "0.6.0", "stable", first);
        QCOMPARE(result.release, first);
        QCOMPARE(UpdateChecker::select_release_page(json({release("0.8.0")}), "0.6.0", "stable", first)
                     .release.value("tag_name")
                     .toString(),
                 "0.8.0");
        // A previous Preview page cannot leak into a Stable selection.
        QVERIFY(UpdateChecker::select_release_page("[]", "0.6.0", "stable", release("0.9.0", true)).release.isEmpty());
    }
    void channel_switch_rejects_reentrant_requests_data() {
        QTest::addColumn<int>("signal");
        QTest::newRow("operation") << 0;
        QTest::newRow("busy") << 1;
        QTest::newRow("cancelled") << 2;
    }
    void channel_switch_rejects_reentrant_requests() {
        QFETCH(int, signal);
        QCoreApplication::setApplicationVersion("0.6.0");
        UpdateChecker checker;
        bool callback_ran = false;
        quint64 check_id = 99;
        quint64 download_id = 99;
        const auto reenter = [&] {
            if (checker.is_busy() || callback_ran)
                return;
            callback_ran = true;
            // Invalid credentials avoid network access even when reproducing the unfixed bug.
            check_id = checker.check("invalid token!");
            download_id = checker.download();
            checker.set_channel("preview");
        };
        if (signal == 0)
            connect(&checker, &UpdateChecker::operation_changed, &checker, reenter);
        else if (signal == 1)
            connect(&checker, &UpdateChecker::busy_changed, &checker, reenter);
        else
            connect(&checker, &UpdateChecker::check_cancelled, &checker, reenter);
        QSignalSpy changed(&checker, &UpdateChecker::channel_changed);
        connect(&checker, &UpdateChecker::check_started, &checker, [&] { checker.set_channel("preview"); });
        checker.check();
        QVERIFY(callback_ran);
        QCOMPARE(check_id, quint64{0});
        QCOMPARE(download_id, quint64{0});
        QCOMPARE(changed.size(), 1);
        QCOMPARE(checker.channel(), "preview");
        QVERIFY(!checker.is_busy());
        QVERIFY(checker.available_version().isEmpty());
        QVERIFY(checker.available_release_url().isEmpty());
        QVERIFY(!checker.can_install());
    }
    void channel_changed_allows_new_request() {
        QCoreApplication::setApplicationVersion("0.6.0");
        UpdateChecker checker;
        quint64 check_id = 0;
        connect(&checker, &UpdateChecker::channel_changed, &checker, [&] {
            QCOMPARE(checker.channel(), "preview");
            check_id = checker.check("invalid token!");
        });
        checker.set_channel("preview");
        QVERIFY(check_id != 0);
        QVERIFY(!checker.is_busy());
    }
    void channel_switch_cancels_check() {
        QCoreApplication::setApplicationVersion("0.6.0");
        UpdateChecker checker;
        QCOMPARE(checker.channel(), "stable");
        QSignalSpy cancelled(&checker, &UpdateChecker::check_cancelled);
        connect(&checker, &UpdateChecker::check_started, &checker, [&] { checker.set_channel("preview"); });
        checker.check(); // Reentrant switch cancels before any network request is made.
        QCOMPARE(cancelled.size(), 1);
        QVERIFY(!checker.is_busy());
        QCOMPARE(checker.channel(), "preview");
        QVERIFY(checker.available_version().isEmpty());
        QVERIFY(!checker.can_install());
        checker.set_channel("unknown");
        QCOMPARE(checker.channel(), "stable");
    }
};
QTEST_GUILESS_MAIN(UpdateCheckerTest)
#include "update_checker_test.moc"
