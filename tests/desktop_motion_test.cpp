#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>
#include <QUrl>
#include <QVariantAnimation>

#include "appearance.h"
#include "DesktopService.h"
#include "MainWindow.h"
#include "PlatformIntegration.h"
#include "SearchResults.h"

namespace {
void capture_widget(QWidget *widget, const QString &filename) {
    const auto directory = qEnvironmentVariable("FORADECK_TEST_CAPTURES");
    if (!directory.isEmpty())
        QVERIFY(widget->grab().save(QDir(directory).filePath(filename)));
}

class TestDesktop {
    QTemporaryDir profile;

public:
    std::unique_ptr<DesktopService> service;
    std::unique_ptr<MainWindow> window;

    TestDesktop() {
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("onboarding/completed", false);
        settings.setValue("appearance/animations", false);
        settings.setValue("appearance/theme", qEnvironmentVariable("FORADECK_TEST_THEME", "dark"));
        const auto scale = qEnvironmentVariableIntValue("FORADECK_TEST_SCALE");
        settings.setValue("appearance/text_scale", scale > 0 ? scale : 100);
        settings.setValue("shortcuts/global", "");
        settings.setValue("shortcuts/overlay", "");
        settings.setValue("search/overlay_rows", 7);
        settings.sync();
        service = std::make_unique<DesktopService>(nullptr, profile.path(), profile.filePath("library.sqlite"));
        window = std::make_unique<MainWindow>(*service);
        window->show();
    }

    QString file(const QString &name, const QByteArray &data) {
        QFile fixture(profile.filePath(name));
        if (!fixture.open(QIODevice::WriteOnly) || fixture.write(data) != data.size())
            return {};
        return fixture.fileName();
    }

    void trigger(const QString &name) {
        if (auto *action = window->findChild<QAction *>(name))
            action->trigger();
    }

    QString application_file(const QString &name) {
#ifdef Q_OS_WIN
        return file(name + ".cmd", "@echo off\r\n");
#else
        const auto path = file(name, "#!/bin/sh\nexit 0\n");
        if (!QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner))
            return {};
        return path;
#endif
    }
};

bool drop_urls(QWidget *target, const QList<QUrl> &urls, Qt::DropActions actions = Qt::CopyAction) {
    QMimeData mime;
    mime.setUrls(urls);
    const auto point = target->rect().center();
    QDragEnterEvent enter(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    if (!enter.isAccepted())
        return false;
    QDropEvent drop(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}

bool choose_file_with_keyboard(QDialog *form, const QString &path) {
    auto *picker = form->findChild<QPushButton *>("drop_file_picker");
    if (!picker)
        return false;
    int attempts = 0;
    bool accepted = false;
    QTimer respond;
    QObject::connect(&respond, &QTimer::timeout, &respond, [&] {
        if (auto *chooser = form->findChild<QFileDialog *>()) {
            if (++attempts > 40) {
                chooser->reject();
                return;
            }
            if (auto *filename = chooser->findChild<QLineEdit *>("fileNameEdit")) {
                filename->setText(path);
                if (chooser->selectedFiles() == QStringList{path}) {
                    static_cast<QDialog *>(chooser)->accept();
                    accepted = chooser->result() == QDialog::Accepted;
                }
            }
        }
    });
    respond.start(50);
    picker->setFocus();
    QTest::keyClick(picker, Qt::Key_Return);
    return accepted;
}
}

class DesktopMotionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qApp->setStyle("Fusion");
        qApp->setApplicationDisplayName("Fora.Deck");
        qApp->setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void document_drop_is_local_and_requires_confirmation() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        desktop.service->navigate("files");
        auto *launcher = desktop.window->findChild<QPushButton *>("contextual_add");
        QVERIFY(launcher);
        QCOMPARE(launcher->text(), QString("Add"));
        QVERIFY(!desktop.window->findChild<QPushButton *>("contextual_add_menu"));
        auto *discover = desktop.window->findChild<QPushButton *>("discover_applications");
        QVERIFY(discover && !discover->menu());
        capture_widget(desktop.window.get(), "my-files.png");
        const auto path = desktop.file("notes.txt", "notes");
        QVERIFY(!path.isEmpty());
        launcher->click();
        auto *intake = desktop.window->findChild<QDialog *>("add_items_dialog");
        QVERIFY(intake);
        capture_widget(intake, "add.png");
        auto *intake_zone = intake->findChild<QWidget *>("file_drop_zone");
        QVERIFY(intake_zone);
        const auto local = QUrl::fromLocalFile(path);
        QVERIFY(drop_urls(intake_zone, {local, local}));
        auto *dialog = desktop.window->findChild<QDialog *>("add_files_dialog");
        QVERIFY(dialog);
        auto *zone = dialog->findChild<QWidget *>("file_drop_zone");
        auto *draft = dialog->findChild<QTreeWidget *>("add_files_draft");
        QVERIFY(zone);
        QVERIFY(draft);
        QTRY_VERIFY(dialog->findChild<QPushButton *>("confirm_add_files")->isEnabled());
        QVERIFY(!drop_urls(zone, {local, QUrl("https://example.invalid/file.txt")}));
        QVERIFY(!drop_urls(zone, {local}, Qt::MoveAction));
        QSignalSpy applications(desktop.service.get(), &DesktopService::application_resolved);
        QSignalSpy previews(desktop.service.get(), &DesktopService::catalog_preview_ready);
        QSignalSpy notices(desktop.service.get(), &DesktopService::saved_files_notice);
        QTRY_COMPARE(draft->topLevelItemCount(), 1);
        capture_widget(dialog, "add-files.png");
        QVERIFY(desktop.service->files->persisted_saved_paths().isEmpty());
        dialog->reject();
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("add_files_dialog"));
        QVERIFY(desktop.service->files->persisted_saved_paths().isEmpty());
        launcher->click();
        intake = desktop.window->findChild<QDialog *>("add_items_dialog");
        QVERIFY(intake);
        QVERIFY(choose_file_with_keyboard(intake, path));
        dialog = desktop.window->findChild<QDialog *>("add_files_dialog");
        QVERIFY(dialog);
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QCOMPARE(buttons->buttons().size(), 2);
        auto *add = dialog->findChild<QPushButton *>("confirm_add_files");
        QVERIFY(add);
        QTRY_VERIFY(add->isEnabled());
        add->click();
        dialog->reject();
        QTRY_COMPARE(notices.size(), 1);
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("add_files_dialog"));
        QCOMPARE(desktop.service->files->persisted_saved_paths(), QStringList{FileManager::saved_path_identity(path)});
        QVERIFY(desktop.service->catalog().applications.isEmpty());
        QVERIFY(!desktop.service->files->is_enabled());
        QCOMPARE(applications.size(), 0);
        QCOMPARE(previews.size(), 0);
    }

    void mixed_selection_opens_both_drafts() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        const auto program = desktop.application_file("test-program");
        const auto notes = desktop.file("notes.txt", "notes");
        QVERIFY(!program.isEmpty() && !notes.isEmpty());
        auto *launcher = desktop.window->findChild<QPushButton *>("contextual_add");
        QVERIFY(launcher);
        launcher->click();
        auto *intake = desktop.window->findChild<QDialog *>("add_items_dialog");
        QVERIFY(intake);
        auto *zone = intake->findChild<QWidget *>("file_drop_zone");
        QVERIFY(zone);
        QVERIFY(drop_urls(zone, {QUrl::fromLocalFile(program), QUrl::fromLocalFile(notes)}));
        auto *files = desktop.window->findChild<QDialog *>("add_files_dialog");
        QVERIFY(files);
        QVERIFY(!desktop.window->findChild<QDialog *>("application_editor"));
        files->reject();
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("add_files_dialog"));
        QVERIFY(!desktop.window->findChild<QDialog *>("application_editor"));

        QVERIFY(drop_urls(desktop.window.get(), {QUrl::fromLocalFile(program), QUrl::fromLocalFile(notes)}));
        files = desktop.window->findChild<QDialog *>("add_files_dialog");
        QVERIFY(files);
        auto *skip = files->findChild<QPushButton *>("skip_add_files");
        QVERIFY(skip);
        skip->click();
        auto *editor = desktop.window->findChild<QDialog *>("application_editor");
        QVERIFY(editor);
        QTRY_VERIFY(!editor->property("resolve_request_id").isValid());
        auto *target = editor->findChild<QLineEdit *>("application_path");
        QVERIFY(target);
        QCOMPARE(FileManager::saved_path_identity(target->text()), FileManager::saved_path_identity(program));
        editor->reject();
        QVERIFY(desktop.service->catalog().applications.isEmpty());
        QVERIFY(desktop.service->files->persisted_saved_paths().isEmpty());
    }

    void application_drop_ignores_other_requests_and_can_be_cancelled() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        const auto path = desktop.application_file("test-program");
        const auto other = desktop.application_file("other-program");
        QVERIFY(!path.isEmpty());
        QVERIFY(!other.isEmpty());
        auto *add = desktop.window->findChild<QAction *>("add_items");
        QVERIFY(add);
        QCOMPARE(add->shortcut(), QKeySequence("Ctrl+N"));
        add->trigger();
        auto *intake = desktop.window->findChild<QDialog *>("add_items_dialog");
        QVERIFY(intake);
        auto *manual = intake->findChild<QPushButton *>("add_application_manually");
        QVERIFY(manual);
        manual->click();
        auto *dialog = desktop.window->findChild<QDialog *>("application_editor");
        QVERIFY(dialog);
        auto *zone = dialog->findChild<QWidget *>("file_drop_zone");
        auto *target = dialog->findChild<QLineEdit *>("application_path");
        QVERIFY(zone);
        QVERIFY(target);
        QVERIFY(!drop_urls(zone, {QUrl::fromLocalFile(path), QUrl::fromLocalFile(other)}));
        QVERIFY(target->text().isEmpty());
        QSignalSpy resolved(desktop.service.get(), &DesktopService::application_resolved);
        desktop.service->resolve_application(other);
        QVERIFY(drop_urls(zone, {QUrl::fromLocalFile(path)}));
        QTRY_COMPARE(resolved.size(), 2);
        QTRY_VERIFY(!dialog->property("resolve_request_id").isValid());
        QCOMPARE(QDir::cleanPath(target->text()), QDir::cleanPath(path));
        QVERIFY(desktop.service->catalog().applications.isEmpty());
        dialog->reject();
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("application_editor"));
        QVERIFY(desktop.service->catalog().applications.isEmpty());
        QVERIFY(desktop.service->files->persisted_saved_paths().isEmpty());

        desktop.trigger("add_application");
        dialog = desktop.window->findChild<QDialog *>("application_editor");
        QVERIFY(dialog);
        QVERIFY(choose_file_with_keyboard(dialog, path));
        target = dialog->findChild<QLineEdit *>("application_path");
        QTRY_VERIFY(!dialog->property("resolve_request_id").isValid());
        QCOMPARE(QDir::cleanPath(target->text()), QDir::cleanPath(path));
        dialog->reject();
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("application_editor"));

        QSignalSpy preview(desktop.service.get(), &DesktopService::catalog_preview_ready);
        QVERIFY(drop_urls(desktop.window.get(), {QUrl::fromLocalFile(path), QUrl::fromLocalFile(other)},
                          Qt::CopyAction | Qt::MoveAction));
        QTRY_COMPARE(preview.size(), 1);
        QVERIFY(desktop.service->catalog().applications.isEmpty());
    }

    void confirmation_saves_the_application_and_preserves_edited_name() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        const auto path = desktop.application_file("test-program");
        QVERIFY(!path.isEmpty());
        desktop.trigger("add_items");
        auto *intake = desktop.window->findChild<QDialog *>("add_items_dialog");
        QVERIFY(intake);
        auto *zone = intake->findChild<QWidget *>("file_drop_zone");
        QVERIFY(zone);
        QVERIFY(drop_urls(zone, {QUrl::fromLocalFile(path)}));
        auto *dialog = desktop.window->findChild<QDialog *>("application_editor");
        QVERIFY(dialog);
        auto *name = dialog->findChild<QLineEdit *>("application_name");
        QVERIFY(name);
        name->setText(QString::fromUtf8("Тестовое приложение"));
        QTRY_VERIFY(!dialog->property("resolve_request_id").isValid());
        QCOMPARE(name->text(), QString::fromUtf8("Тестовое приложение"));
        capture_widget(dialog, "add-application.png");
        QVERIFY(desktop.service->catalog().applications.isEmpty());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("application_editor"));
        QTRY_COMPARE(desktop.service->catalog().applications.size(), 1);
        const auto &record = desktop.service->catalog().applications.first();
        QCOMPARE(record.name, QString::fromUtf8("Тестовое приложение"));
        QCOMPARE(FileManager::saved_path_identity(record.path), FileManager::saved_path_identity(path));

        desktop.service->select(record.id);
        auto *favorite = desktop.window->findChild<QPushButton *>("inspector_favorite");
        QVERIFY(favorite);
        if (!favorite->isVisible()) {
            desktop.trigger("details");
            auto *details = desktop.window->findChild<QDialog *>("details_dialog");
            QVERIFY(details);
            favorite = details->findChild<QPushButton *>("inspector_favorite");
        }
        QVERIFY(favorite && favorite->isVisible());
        auto *motion = favorite->findChild<QVariantAnimation *>();
        QVERIFY(motion);
        desktop.service->settings.setValue("appearance/animations", true);
        emit desktop.service->appearance_changed();
        QSignalSpy motion_states(motion, &QAbstractAnimation::stateChanged);
        favorite->click();
        QTRY_VERIFY(desktop.service->selected() && desktop.service->selected()->favorite);
        if (desktop_platform::animations_enabled()) {
            QVERIFY(std::any_of(motion_states.cbegin(), motion_states.cend(), [](const QList<QVariant> &state) {
                return state.first().value<QAbstractAnimation::State>() == QAbstractAnimation::Running;
            }));
        } else {
            QVERIFY(motion_states.isEmpty());
        }
        QCOMPARE(favorite->accessibleName(), QString("Remove from favorites"));
        capture_widget(favorite->window(), "application-details.png");
    }

    void search_hint_tracks_remapped_shortcut() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        auto *search = desktop.window->findChild<QLineEdit *>("search_input");
        auto *action = desktop.window->findChild<QAction *>("focus_search");
        QVERIFY(search);
        auto *hint = search->findChild<QLabel *>("search_shortcut_hint");
        QVERIFY(action);
        QVERIFY(hint);
        const auto updated = QKeySequence("Ctrl+Shift+K");
        action->setShortcut(updated);
        QCOMPARE(hint->text(), updated.toString(QKeySequence::NativeText));
        search->setText(QString::fromUtf8("пример"));
        QVERIFY(hint->isHidden());
        search->clear();
        QCOMPARE(!hint->isHidden(), search->textMargins().right() > 0);
        action->setShortcut({});
        QVERIFY(hint->isHidden());
    }

    void mode_switch_keeps_toolbar_buttons_in_place() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        auto exclusions = FileManager::volume_roots();
        for (const auto location :
             {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation})
            exclusions.append(QStandardPaths::writableLocation(location));
        desktop.service->files->configure(file_search_mode::basic, {}, exclusions);
        auto *group = desktop.window->findChild<QWidget *>("file_search_switch");
        auto *add = desktop.window->findChild<QPushButton *>("contextual_add");
        auto *discover = desktop.window->findChild<QPushButton *>("discover_applications");
        QVERIFY(group);
        QVERIFY(add);
        QVERIFY(discover);
        QTRY_VERIFY(group->isVisible());
        auto *applications = group->findChild<QPushButton *>("search_applications");
        auto *files = group->findChild<QPushButton *>("search_files");
        QVERIFY(applications);
        QVERIFY(files);

        QCoreApplication::processEvents();
        const auto geometry = [&](const QWidget *widget) {
            return QRect(widget->mapTo(desktop.window.get(), QPoint{}), widget->size());
        };
        const auto group_geometry = geometry(group);
        const auto add_geometry = geometry(add);
        const auto discover_geometry = geometry(discover);
        QVERIFY(add->width() >= add->sizeHint().width());
        files->click();
        QVERIFY(desktop.service->file_search_active());
        QCoreApplication::processEvents();
        QCOMPARE(geometry(group), group_geometry);
        QCOMPARE(geometry(add), add_geometry);
        QCOMPARE(geometry(discover), discover_geometry);
        QVERIFY(add->width() >= add->sizeHint().width());

        applications->click();
        QVERIFY(!desktop.service->file_search_active());
        QCoreApplication::processEvents();
        QCOMPARE(geometry(group), group_geometry);
        QCOMPARE(geometry(add), add_geometry);
        QCOMPARE(geometry(discover), discover_geometry);
    }

    void mode_pill_reverses_from_the_current_frame() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        auto exclusions = FileManager::volume_roots();
        for (const auto location :
             {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation})
            exclusions.append(QStandardPaths::writableLocation(location));
        desktop.service->files->configure(file_search_mode::basic, {}, exclusions);
        auto *group = desktop.window->findChild<QWidget *>("file_search_switch");
        QVERIFY(group);
        auto *files = group->findChild<QPushButton *>("search_files");
        auto *applications = group->findChild<QPushButton *>("search_applications");
        auto *motion = group->findChild<QVariantAnimation *>();
        QVERIFY(files);
        QVERIFY(applications);
        QVERIFY(motion);
        QTRY_VERIFY(group->isVisible());
        QCoreApplication::processEvents();
        desktop.service->settings.setValue("appearance/animations", true);
        emit desktop.service->appearance_changed();
        QCoreApplication::processEvents();
        files->click();
        QVERIFY(desktop.service->file_search_active());
        QVERIFY(files->isChecked());
        if (desktop_platform::animations_enabled()) {
            QCOMPARE(motion->state(), QAbstractAnimation::Running);
            motion->setCurrentTime(motion->duration() / 3);
            const auto frame = motion->currentValue().toRect();
            QVERIFY(frame != applications->geometry());
            QVERIFY(frame != files->geometry());
            applications->click();
            QCOMPARE(motion->startValue().toRect(), frame);
        } else {
            applications->click();
        }
        QVERIFY(!desktop.service->file_search_active());
        QVERIFY(applications->isChecked());
        desktop.service->settings.setValue("appearance/animations", false);
        emit desktop.service->appearance_changed();
        QTRY_COMPARE(motion->state(), QAbstractAnimation::Stopped);
        QTRY_COMPARE(motion->currentValue().toRect(), applications->geometry());
        desktop.service->files->configure(file_search_mode::off);
        QVERIFY(group->isHidden());
    }

    void file_limit_applies_on_save_and_results_survive_a_tab_switch() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        QStringList paths;
        for (int index = 0; index < 4; ++index) {
            const auto path = desktop.file(QString("limit-fixture-%1.txt").arg(index), "notes");
            QVERIFY(!path.isEmpty());
            paths.append(path);
        }
        QStringList exclusions;
        for (const auto location :
             {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation})
            exclusions.append(QStandardPaths::writableLocation(location));
        desktop.service->settings.setValue("files/mode", "basic");
        desktop.service->settings.setValue("files/roots", QStringList{});
        desktop.service->settings.setValue("files/exclusions", exclusions);
        desktop.service->files->configure(file_search_mode::basic, {}, exclusions);
        bool saved = false;
        QObject receiver;
        desktop.service->files->save_files(
            paths, &receiver, [&](const file_save_result &result) { saved = result.added.size() == paths.size(); });
        QTRY_VERIFY(saved);

        auto *search = desktop.window->findChild<QLineEdit *>("search_input");
        auto *group = desktop.window->findChild<QWidget *>("file_search_switch");
        auto *view = desktop.window->findChild<SearchResults *>("file_list");
        QVERIFY(search && group && view);
        auto *files = group->findChild<QPushButton *>("search_files");
        auto *applications = group->findChild<QPushButton *>("search_applications");
        QVERIFY(files && applications);
        search->setText("limit-fixture-*.txt");
        files->click();
        QTRY_COMPARE(view->result_count(), 4);
        applications->click();
        files->click();
        QCOMPARE(view->result_count(), 4);
        QVERIFY(view->currentIndex().isValid());

        for (const auto action : {QDialogButtonBox::Cancel, QDialogButtonBox::Save}) {
            desktop.trigger("settings");
            auto *dialog = desktop.window->findChild<QDialog *>("settings");
            QVERIFY(dialog);
            auto *section = dialog->findChild<QAbstractButton *>("settings_files_category");
            QVERIFY(section);
            section->click();
            auto *preset = dialog->findChild<QComboBox *>("file_result_limit_preset");
            auto *manual = dialog->findChild<QSpinBox *>("file_result_limit");
            QVERIFY(preset && manual);
            QCOMPARE(preset->currentData().toInt(), 200);
            preset->setCurrentIndex(preset->findData(500));
            QCOMPARE(manual->value(), 500);
            QVERIFY(manual->isHidden());
            preset->setCurrentIndex(preset->findData(0));
            QVERIFY(manual->isVisible());
            manual->setValue(2);
            dialog->findChild<QDialogButtonBox *>()->button(action)->click();
            QTRY_VERIFY(!desktop.window->findChild<QDialog *>("settings"));
            QTRY_COMPARE(view->result_count(), action == QDialogButtonBox::Save ? 2 : 4);
        }
        desktop.trigger("settings");
        auto *dialog = desktop.window->findChild<QDialog *>("settings");
        QVERIFY(dialog);
        auto *preset = dialog->findChild<QComboBox *>("file_result_limit_preset");
        auto *manual = dialog->findChild<QSpinBox *>("file_result_limit");
        QVERIFY(preset && manual);
        QCOMPARE(preset->currentData().toInt(), 0);
        QCOMPARE(manual->value(), 2);
        dialog->reject();
    }

    void overlay_rows_follow_save_and_cancel() {
        TestDesktop desktop;
        QTRY_VERIFY(desktop.service->is_ready());
        for (const auto button : {QDialogButtonBox::Cancel, QDialogButtonBox::Save}) {
            desktop.trigger("settings");
            auto *dialog = desktop.window->findChild<QDialog *>("settings");
            QVERIFY(dialog);
            auto *section = dialog->findChild<QAbstractButton *>("settings_appearance_category");
            QVERIFY(section);
            section->click();
            auto *slider = dialog->findChild<QSlider *>("overlay_rows");
            QVERIFY(slider);
            QCOMPARE(slider->value(), 7);
            QTest::keyClick(slider, button == QDialogButtonBox::Save ? Qt::Key_End : Qt::Key_Home);
            QCOMPARE(slider->value(), button == QDialogButtonBox::Save ? 10 : 3);
            if (button == QDialogButtonBox::Save)
                capture_widget(dialog, "overlay-settings.png");
            dialog->findChild<QDialogButtonBox *>()->button(button)->click();
            QTRY_VERIFY(!desktop.window->findChild<QDialog *>("settings"));
            QCOMPARE(desktop.service->settings.value("search/overlay_rows").toInt(),
                     button == QDialogButtonBox::Save ? 10 : 7);
        }
        desktop.trigger("settings");
        auto *dialog = desktop.window->findChild<QDialog *>("settings");
        QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QSlider *>("overlay_rows")->value(), 10);
        dialog->reject();
        QTRY_VERIFY(!desktop.window->findChild<QDialog *>("settings"));
        for (int index = 0; index < 11; ++index) {
            app_record record;
            record.path = desktop.application_file(QString("motion-suite-%1").arg(index));
            record.name = QString("Motion suite %1").arg(index);
            QVERIFY(!record.path.isEmpty());
            desktop.service->save_application(std::move(record));
        }
        QTRY_COMPARE(desktop.service->catalog().applications.size(), 11);
        desktop.window->show_overlay();
        QWidget *overlay = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->objectName() == "search_overlay")
                overlay = widget;
        QVERIFY(overlay);
        auto *input = overlay->findChild<QLineEdit *>("overlay_search_input");
        auto *results = overlay->findChild<SearchResults *>();
        QVERIFY(input);
        QVERIFY(results);
        input->setText("Motion suite");
        QTRY_COMPARE(results->model()->rowCount(), 11);
        QCOMPARE(results->property("preferred_height").toInt(),
                 10 * appearance::scaled(desktop.service->settings, 60) + 4);
        capture_widget(overlay, "overlay.png");
    }
};

QTEST_MAIN(DesktopMotionTest)
#include "desktop_motion_test.moc"
