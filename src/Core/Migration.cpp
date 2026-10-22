#include "Migration.hpp"
#include "Logging.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>

#if __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <qt6keychain/keychain.h>
#endif

namespace Styx {
namespace Core {

namespace {

bool copyDirectoryRecursively(const QString &sourcePath, const QString &targetPath)
{
    QDir sourceDir(sourcePath);
    if (!sourceDir.exists())
        return false;

    QDir targetDir(targetPath);
    if (!targetDir.exists())
        targetDir.mkpath(".");

    const QFileInfoList entries = sourceDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString srcItem = entry.filePath();
        const QString dstItem = targetDir.filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectoryRecursively(srcItem, dstItem))
                return false;
        } else {
            if (!QFile::exists(dstItem)) {
                if (!QFile::copy(srcItem, dstItem))
                    return false;
            }
        }
    }
    return true;
}

} // namespace

bool Migration::isMigrationDone()
{
    const QString markerPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(".migration_done");
    return QFile::exists(markerPath);
}

void Migration::writeMigrationMarker()
{
    const QString newRoamPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(newRoamPath).mkpath(".");
    const QString markerPath = QDir(newRoamPath).filePath(".migration_done");
    QFile markerFile(markerPath);
    if (markerFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&markerFile);
        out << "Migrated on " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        markerFile.close();
    }
}

void Migration::migrateSettingsAndData()
{
    if (isMigrationDone())
        return;

    const QString oldRoamPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/ouwou/Acheron";
    const QString newRoamPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    const QString oldLocalPath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/ouwou/Acheron";
    const QString newLocalPath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/larrythemobster/Styx";

    QSettings oldSettings("ouwou", "Acheron");
    const QStringList oldKeys = oldSettings.allKeys();
    const bool oldRoamExists = QDir(oldRoamPath).exists();
    const bool oldLocalExists = QDir(oldLocalPath).exists();
    const bool oldSettingsExist = !oldKeys.isEmpty();

    if (!oldRoamExists && !oldLocalExists && !oldSettingsExist) {
        writeMigrationMarker();
        return;
    }

    qCInfo(LogCore) << "Migrating Acheron data to Styx...";

    if (oldSettingsExist) {
        QSettings newSettings("larrythemobster", "Styx");
        int migratedKeys = 0;
        for (const QString &key : oldKeys) {
            QVariant val = oldSettings.value(key);
            QString newKey = key;
            newKey.replace("acheron", "styx", Qt::CaseInsensitive);
            newKey.replace("ouwou", "larrythemobster", Qt::CaseInsensitive);

            if (val.userType() == QMetaType::QString) {
                QString s = val.toString();
                s.replace("acheron", "styx", Qt::CaseInsensitive);
                s.replace("ouwou", "larrythemobster", Qt::CaseInsensitive);
                val = s;
            } else if (val.userType() == QMetaType::QStringList) {
                QStringList list = val.toStringList();
                for (QString &item : list) {
                    item.replace("acheron", "styx", Qt::CaseInsensitive);
                    item.replace("ouwou", "larrythemobster", Qt::CaseInsensitive);
                }
                val = list;
            }
            newSettings.setValue(newKey, val);
            migratedKeys++;
        }
        newSettings.sync();
        qCInfo(LogCore) << "Migrated" << migratedKeys << "settings keys from Acheron.";
    }

    if (oldRoamExists) {
        QDir newRoamDir(newRoamPath);
        if (!newRoamDir.exists())
            newRoamDir.mkpath(".");

        QDir oldRoamDir(oldRoamPath);
        const QFileInfoList files = oldRoamDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &fileInfo : files) {
            const QString fileName = fileInfo.fileName();
            QString destName = fileName;
            if (fileName == "acheron.sqlite")
                destName = "styx.sqlite";
            else if (fileName == "acheron.sqlite-wal")
                destName = "styx.sqlite-wal";
            else if (fileName == "acheron.sqlite-shm")
                destName = "styx.sqlite-shm";
            else if (fileName == "acheron.log")
                destName = "styx.log";
            else if (fileName.startsWith("acheron.log."))
                destName = "styx.log." + fileName.mid(12);

            const QString dst = newRoamDir.filePath(destName);
            if (!QFile::exists(dst)) {
                if (QFile::copy(fileInfo.filePath(), dst)) {
                    qCInfo(LogCore) << "Migrated AppData file:" << fileName << "->" << destName;
                }
            }
        }
    }

    if (oldLocalExists) {
        QDir newLocalDir(newLocalPath);
        if (!newLocalDir.exists())
            newLocalDir.mkpath(".");

        if (copyDirectoryRecursively(oldLocalPath, newLocalPath)) {
            qCInfo(LogCore) << "Migrated LocalAppData cache directory.";
        }
    }
}

void Migration::migrateKeychain()
{
    if (isMigrationDone())
        return;

    const QString newRoamPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dbPath = QDir(newRoamPath).filePath("styx.sqlite");

    if (QFile::exists(dbPath)) {
        const QString migrationConn = "Styx_Migration_DB";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", migrationConn);
            db.setDatabaseName(dbPath);
            if (db.open()) {
                QSqlQuery query(db);
                if (query.exec("SELECT id FROM accounts")) {
                    while (query.next()) {
                        quint64 accountId = query.value(0).toULongLong();
                        QString key = QString("account_%1_token").arg(accountId);

                        QKeychain::ReadPasswordJob readJob("Acheron");
                        readJob.setAutoDelete(false);
                        readJob.setKey(key);

                        QEventLoop loop;
                        QObject::connect(&readJob, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
                        readJob.start();
                        loop.exec();

                        if (readJob.error() == QKeychain::NoError) {
                            QString token = readJob.textData();
                            if (!token.isEmpty()) {
                                QKeychain::WritePasswordJob writeJob("Styx");
                                writeJob.setAutoDelete(false);
                                writeJob.setKey(key);
                                writeJob.setTextData(token);

                                QEventLoop writeLoop;
                                QObject::connect(&writeJob, &QKeychain::Job::finished, &writeLoop, &QEventLoop::quit);
                                writeJob.start();
                                writeLoop.exec();

                                if (writeJob.error() == QKeychain::NoError) {
                                    qCInfo(LogCore) << "Migrated keychain token for account" << accountId;
                                } else {
                                    qCWarning(LogCore) << "Failed to store migrated keychain token for account"
                                                       << accountId << ":" << writeJob.errorString();
                                }
                            }
                        }
                    }
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(migrationConn);
    }

    writeMigrationMarker();
    qCInfo(LogCore) << "Migration from Acheron completed.";
}

} // namespace Core
} // namespace Styx
