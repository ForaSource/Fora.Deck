#include <memory>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "Database.h"
#include "desktop/appearance.h"
#include "desktop/DesktopService.h"
#include "desktop/localization.h"
#include "desktop/MainWindow.h"
#include "desktop/PlatformIntegration.h"
#include "src/ApplicationPaths.h"
#include "src/PathHelpers.h"
#include "src/ProfileBackup.h"

int main(int argc, char *argv[]) {
#ifdef Q_OS_LINUX
    // Capture before Qt can consume the startup token while creating native windows.
    const auto activation_token = qgetenv("XDG_ACTIVATION_TOKEN");
    qunsetenv("XDG_ACTIVATION_TOKEN");
#endif
    QApplication application(argc, argv);
    Localization localization;
    localization.apply();
    application.setOrganizationName("XLibrary");
    application.setApplicationName("XLibrary");
    application.setApplicationVersion(XLIBRARY_VERSION);
    application.setWindowIcon(appearance::logo());
    application.setQuitOnLastWindowClosed(false);
    application.setStyle("Fusion");
    const bool check_startup = application.arguments().contains("--check-startup");
    QJsonObject invocation{{"command", application.arguments().contains("--overlay") ? "overlay" : "show"}};
    for (const auto &option : {QString("--launch-id"), QString("--scenario-id")}) {
        const auto position = application.arguments().indexOf(option);
        if (position < 0)
            continue;
        bool valid = false;
        const auto id = application.arguments().value(position + 1).toLongLong(&valid);
        if (!valid || id <= 0 || invocation.value("command") != "show") {
            qCritical("Specify one command and a positive persistent target ID.");
            return 2;
        }
        invocation.insert("command", option == "--launch-id" ? "launch" : "scenario");
        invocation.insert("id", QString::number(id));
    }
#ifdef Q_OS_LINUX
    if (activation_token.size() <= 4096 && !activation_token.contains('\0'))
        invocation.insert("activation_token", QString::fromUtf8(activation_token));
#endif
    const bool portable = application_paths::portable_mode();
    std::unique_ptr<QTemporaryDir> startup_profile;
    try {
        if (check_startup) {
            startup_profile = std::make_unique<QTemporaryDir>();
            if (!startup_profile->isValid())
                throw std::runtime_error(QCoreApplication::translate(
                                             "XLibrary", "Could not create a temporary profile for the startup check.")
                                             .toStdString());
        }
        const auto appimage = check_startup ? QString{} : application_paths::appimage_file();
        const auto profile = check_startup         ? startup_profile->path()
                             : !appimage.isEmpty() ? appimage + ".data"
                             : portable            ? application.applicationDirPath() + "/data"
                                        : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!QDir().mkpath(profile))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not create the XLibrary settings folder.")
                    .toStdString());
        const auto profile_identity = normalized_path(QFileInfo(profile).canonicalFilePath());
        const auto server_name =
            "xlibrary-" +
            QString::fromLatin1(
                QCryptographicHash::hash(profile_identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        QLockFile instance_lock(profile + "/instance.lock");
        instance_lock.setStaleLockTime(0);
        if (!instance_lock.tryLock(application.arguments().contains("--wait-for-profile") ? 5000 : 0)) {
            if (instance_lock.error() != QLockFile::LockFailedError)
                throw std::runtime_error(
                    QCoreApplication::translate(
                        "XLibrary", "Could not write the XLibrary profile lock file. Check access permissions.")
                        .toStdString());
            QLocalSocket socket;
            socket.connectToServer(server_name);
            if (socket.waitForConnected(1500)) {
                socket.write(QJsonDocument(invocation).toJson(QJsonDocument::Compact) + '\n');
                socket.flush();
                socket.waitForBytesWritten(100);
                return 0;
            }
            QMessageBox::information(
                nullptr, "XLibrary",
                QCoreApplication::translate("XLibrary", "Another instance of XLibrary is starting or not responding."));
            return 1;
        }
        const auto database_path =
            (portable || check_startup) ? profile + "/xlibrary.db" : path_text(Database::get_database_path());
        ProfileBackup::recover(database_path, profile + "/settings.ini");
        QSettings language_settings(profile + "/settings.ini", QSettings::IniFormat);
        localization.apply(language_settings.value("interface/language", "system").toString());
        QString reconciliation_error;
        if (language_settings.value("profile/autostart_pending", false).toBool()) {
            if (desktop_platform::set_autostart(language_settings.value("behavior/autostart", false).toBool(),
                                                reconciliation_error)) {
                language_settings.remove("profile/autostart_pending");
                language_settings.sync();
            }
        }
        QLocalServer::removeServer(server_name);
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(server_name))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not open the XLibrary local channel.").toStdString());
        // Portable mode and startup checks use their own directory without replacing the child process environment.
        DesktopService service(nullptr, profile, database_path);
        if (!service.settings.isWritable())
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Cannot write XLibrary settings. Choose a writable folder.")
                    .toStdString());
        MainWindow window(service);
        const auto handle_invocation = [&window, &service](const QJsonObject &request) {
            const auto command = request.value("command").toString();
            if (command == "show" || command == "overlay") {
#ifdef Q_OS_LINUX
                const auto token = request.value("activation_token").toString().toUtf8();
                qunsetenv("XDG_ACTIVATION_TOKEN");
                if (QGuiApplication::platformName().startsWith("wayland") && !token.isEmpty() && token.size() <= 4096 &&
                    !token.contains('\0'))
                    qputenv("XDG_ACTIVATION_TOKEN", token);
#endif
                if (command == "overlay")
                    window.show_overlay();
                else
                    window.show_library();
#ifdef Q_OS_LINUX
                // Tokens authorize one activation and must not leak to a later request or child process.
                qunsetenv("XDG_ACTIVATION_TOKEN");
#endif
            } else if (command == "launch" || command == "scenario") {
                bool valid = false;
                const auto id = request.value("id").toString().toLongLong(&valid);
                if (!valid || id <= 0)
                    return;
                const auto run = [&service, id, command] {
                    if (command == "launch")
                        service.launch(id);
                    else
                        service.run_scenario(id);
                };
                if (service.is_ready())
                    run();
                else {
                    auto connection = std::make_shared<QMetaObject::Connection>();
                    *connection = QObject::connect(&service, &DesktopService::catalog_changed, &service,
                                                   [&service, connection, run] {
                                                       if (service.is_ready()) {
                                                           QObject::disconnect(*connection);
                                                           run();
                                                       }
                                                   });
                }
            }
        };
        if (!reconciliation_error.isEmpty())
            QTimer::singleShot(0, &service, [&service, reconciliation_error] { service.notify(reconciliation_error); });
        if (check_startup) {
            QObject::connect(&service, &DesktopService::catalog_changed, &application, [&] {
                if (service.is_ready()) {
                    QTimer::singleShot(0, &application, [&] { application.exit(window.isVisible() ? 0 : 1); });
                }
            });
            QObject::connect(&service, &DesktopService::message, &application, [&](const QString &message) {
                qCritical("%s", qPrintable(message));
                application.exit(1);
            });
            QTimer::singleShot(30000, &application, [&] {
                qCritical("Startup check timed out.");
                application.exit(1);
            });
        }
        QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                const auto read_request = [socket, request, handle_invocation] {
                    *request += socket->readAll();
                    if (request->size() > 16384) {
                        socket->disconnectFromServer();
                        return;
                    }
                    if (*request == "overlay" || *request == "show")
                        handle_invocation({{"command", QString::fromLatin1(*request)}});
                    else if (request->contains('\n')) {
                        const auto document = QJsonDocument::fromJson(request->left(request->indexOf('\n')));
                        if (document.isObject())
                            handle_invocation(document.object());
                    } else
                        return;
                    socket->disconnectFromServer();
                    socket->deleteLater();
                };
                QObject::connect(socket, &QLocalSocket::readyRead, &window, read_request);
                QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(1500, socket, [socket] {
                    socket->disconnectFromServer();
                    socket->deleteLater();
                });
                read_request();
            }
        });
#ifdef Q_OS_LINUX
        if (QGuiApplication::platformName().startsWith("wayland") && !activation_token.isEmpty() &&
            activation_token.size() <= 4096 && !activation_token.contains('\0'))
            qputenv("XDG_ACTIVATION_TOKEN", activation_token);
#endif
        window.start();
#ifdef Q_OS_LINUX
        qunsetenv("XDG_ACTIVATION_TOKEN");
#endif
        if (invocation.value("command") == "launch" || invocation.value("command") == "scenario")
            handle_invocation(invocation);
        return application.exec();
    } catch (const std::exception &error) {
        if (check_startup)
            qCritical("%s", error.what());
        else
            QMessageBox::critical(nullptr, QCoreApplication::translate("XLibrary", "XLibrary error"),
                                  QString::fromUtf8(error.what()));
        return 1;
    }
}
