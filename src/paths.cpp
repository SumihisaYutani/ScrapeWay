#include "paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QSettings>
#include <QStandardPaths>

namespace paths {
namespace {

QMutex mutex;

QSettings& settings()
{
    static QSettings s(dataDir() + "/settings.ini", QSettings::IniFormat);
    return s;
}

}  // namespace

QString dataDir()
{
    static const QString dir = [] {
        const QString exeDir = QCoreApplication::applicationDirPath();
        const QString d = QFileInfo::exists(exeDir + "/portable.txt")
            ? exeDir + "/data"
            : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

QString profileDir() { return dataDir() + "/profiles/pixiv"; }

QString sessionFile() { return dataDir() + "/pixiv_session.json"; }

QString libraryRoot()
{
    QMutexLocker lock(&mutex);
    const QString def = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ScrapeWay";
    return QDir::cleanPath(settings().value("libraryDir", def).toString());
}

void setLibraryRoot(const QString& dir)
{
    QMutexLocker lock(&mutex);
    settings().setValue("libraryDir", QDir::cleanPath(dir));
}

int defaultWait()
{
    QMutexLocker lock(&mutex);
    return settings().value("wait", 3).toInt();
}

void setDefaultWait(int sec)
{
    QMutexLocker lock(&mutex);
    settings().setValue("wait", sec);
}

}  // namespace paths
