#include "Logging.hpp"

#include <QDateTime>
#include <QDir>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <iostream>

Q_LOGGING_CATEGORY(LogCore, "styx.core");
Q_LOGGING_CATEGORY(LogNetwork, "styx.network");
Q_LOGGING_CATEGORY(LogDiscord, "styx.discord");
Q_LOGGING_CATEGORY(LogDB, "styx.db");
Q_LOGGING_CATEGORY(LogUI, "styx.ui");
Q_LOGGING_CATEGORY(LogProto, "styx.proto");
#ifndef STYX_NO_VOICE
Q_LOGGING_CATEGORY(LogDave, "styx.dave");
#endif
Q_LOGGING_CATEGORY(LogMiniaudio, "styx.miniaudio");
Q_LOGGING_CATEGORY(LogVoice, "styx.voice");
Q_LOGGING_CATEGORY(LogVideo, "styx.video");
Q_LOGGING_CATEGORY(LogStream, "styx.stream");

namespace Styx {
namespace Core {

void Logger::init()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(path);
    if (!dir.exists())
        dir.mkpath(".");

    logFilePath = dir.filePath("styx.log");

    logFile = new QFile(logFilePath);
    if (logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        if (logFile->size() >= maxLogFileSize)
            rotateLogFile();

        QTextStream stream(logFile);
        stream << "\n=== Styx startup: " << QDateTime::currentDateTime().toString() << " ===\n";
    }

    stopping = false;
    writerThread = new std::thread(&Logger::writerLoop);

    qInstallMessageHandler(Logger::messageHandler);
}

void Logger::rotateLogFile()
{
    bool wasOpen = logFile && logFile->isOpen();
    if (wasOpen)
        logFile->close();

    QFile::remove(QString("%1.%2").arg(logFilePath).arg(maxBackupFiles));

    for (int i = maxBackupFiles - 1; i >= 1; i--) {
        QString src = QString("%1.%2").arg(logFilePath).arg(i);
        QString dst = QString("%1.%2").arg(logFilePath).arg(i + 1);
        if (QFile::exists(src))
            QFile::rename(src, dst);
    }

    if (QFile::exists(logFilePath))
        QFile::rename(logFilePath, logFilePath + ".1");

    if (wasOpen && logFile)
        logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
}

void Logger::cleanup()
{
    qInstallMessageHandler(nullptr);

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }
    queueCv.notify_one();

    if (writerThread) {
        if (writerThread->joinable())
            writerThread->join();
        delete writerThread;
        writerThread = nullptr;
    }

    if (logFile) {
        logFile->close();
        delete logFile;
        logFile = nullptr;
    }
}

void Logger::writerLoop()
{
    std::vector<std::string> batch;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [] { return !queue.empty() || stopping; });
            batch.swap(queue);
        }

        if (logFile && logFile->isOpen()) {
            for (const auto &line : batch) {
                logFile->write(line.data(), line.size());
                logFile->write("\n", 1);
            }
            logFile->flush();

            if (logFile->size() >= maxLogFileSize)
                rotateLogFile();
        }
        batch.clear();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stopping && queue.empty())
                break;
        }
    }
}

void Logger::messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const char *level;
    switch (type) {
    case QtDebugMsg:
        level = "DBG";
        break;
    case QtInfoMsg:
        level = "INF";
        break;
    case QtWarningMsg:
        level = "WRN";
        break;
    case QtCriticalMsg:
        level = "CRT";
        break;
    case QtFatalMsg:
        level = "FTL";
        break;
    }

    QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
#ifdef _WIN32
    QString threadId = QString::number(static_cast<quint64>(::GetCurrentThreadId()), 16);
#else
    QString threadId = QString::number(static_cast<quint64>(std::hash<std::thread::id>{}(std::this_thread::get_id())), 16);
#endif

    QString formatted =
            QString("[%1] [T:%2] [%3] [%4] %5").arg(time, threadId, level, context.category, msg);

    std::cout << formatted.toStdString() << std::endl;

    if (type == QtFatalMsg) {
        if (logFile && logFile->isOpen()) {
            auto utf8 = formatted.toUtf8();
            logFile->write(utf8);
            logFile->write("\n", 1);
            logFile->flush();
        }
        abort();
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back(formatted.toUtf8().toStdString());
    }
    queueCv.notify_one();
}

} // namespace Core
} // namespace Styx
