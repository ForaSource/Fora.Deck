#include <memory>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "Database.h"
#include "desktop/Appearance.h"
#include "desktop/DesktopService.h"
#include "desktop/Localization.h"
#include "desktop/MainWindow.h"
#include "desktop/PlatformIntegration.h"
#include "src/ApplicationPaths.h"
#include "src/PathHelpers.h"
#include "src/ProfileBackup.h"

int main(int argc, char *argv[]) {
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
                socket.write(application.arguments().contains("--overlay") ? "overlay" : "show");
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
                const auto read_request = [socket, request, &window] {
                    *request += socket->readAll();
                    if (*request == "overlay")
                        window.show_overlay();
                    else if (*request == "show")
                        window.show_library();
                    else if (request->size() < 7)
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
        window.start();
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
