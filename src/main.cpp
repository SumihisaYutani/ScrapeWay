#include "mainwindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ScrapeWay");
    QApplication::setApplicationVersion(SCRAPEWAY_VERSION);

    MainWindow w;
    w.show();
    // ScrapeWay.exe <URL> で起動したらすぐ読み込む
    if (const QStringList args = app.arguments(); args.size() > 1)
        w.loadUrl(args[1]);
    return app.exec();
}
