// 開発用: CdpSocket で起動中の Chrome に接続して Storage.getCookies を呼ぶ
//   cdptest ws://127.0.0.1:<port>/devtools/browser/<id>
#include "../src/cdpsocket.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    CdpSocket ws;
    QObject::connect(&ws, &CdpSocket::connected, [&] {
        std::printf("connected\n");
        ws.sendTextMessage(R"({"id":1,"method":"Storage.getCookies"})");
    });
    QObject::connect(&ws, &CdpSocket::textMessageReceived, [&](const QString& t) {
        std::printf("message %lld bytes: %s\n", (long long)t.size(), t.left(200).toUtf8().constData());
        app.quit();
    });
    QTimer::singleShot(5000, [&] {
        std::printf("timeout\n");
        app.exit(1);
    });
    ws.open(QUrl(app.arguments().value(1)));
    return app.exec();
}
